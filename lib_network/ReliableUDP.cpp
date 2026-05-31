#include "ReliableUDP.h"
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <pthread.h>

using asio::ip::udp;

static std::once_flag fec_init_flag;

const size_t UsrQueueAsync::MAX_JITTER_DEPTH = 8;
const size_t UsrQueueAsync::MAX_REORDER_BUF = 256;
const std::chrono::milliseconds UsrQueueAsync::FLUSH_TIMEOUT{200};

struct FECLayout
{
    TRXFECMode mode;
    uint8_t data_packets_count;
    uint8_t fec_packets_count;
};

static double calc_rate_bps(std::atomic<uint64_t> &total_bytes, std::mutex &rate_mutex,
                            std::chrono::steady_clock::time_point &last_time, uint64_t &last_bytes)
{
    const auto now = std::chrono::steady_clock::now();

    std::lock_guard<std::mutex> lock(rate_mutex);
    const auto elapsed_seconds = std::chrono::duration<double>(now - last_time).count();
    if (elapsed_seconds <= 0.0)
    {
        return 0.0;
    }

    const auto current_bytes = total_bytes.load(std::memory_order_relaxed);
    const auto delta_bytes = current_bytes - last_bytes;

    last_time = now;
    last_bytes = current_bytes;

    return static_cast<double>(delta_bytes) * 8.0 / elapsed_seconds;
}

static FECLayout resolve_fec_layout(uint8_t units_num)
{
    switch (units_num)
    {
    case 1:
        return {TRXFECMode::None, 1, 0};
    case 2:
        return {TRXFECMode::XOR, 1, 1};
    case MAX_RS_PACKET_NUM_PER_GROUP + TRX_RS_FEC_REDUNDANCY:
        return {TRXFECMode::RS, MAX_RS_PACKET_NUM_PER_GROUP, TRX_RS_FEC_REDUNDANCY};
    default:
        return {TRXFECMode::RS, MAX_RS_PACKET_NUM_PER_GROUP, TRX_RS_FEC_REDUNDANCY};
    }
}

void set_current_thread_scheduler_policy()
{
    auto thread = pthread_self();
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_RR) - 1;
    pthread_setschedparam(thread, SCHED_RR, &param);
    pthread_setname_np(thread, "video_thd");
}

// BackgroundService implementation
BackgroundService &BackgroundService::instance()
{
    static BackgroundService instance;
    return instance;
}

BackgroundService::BackgroundService() : work_guard(asio::make_work_guard(io_context))
{
    for (size_t i = 0; i < 2; ++i)
    {
        io_thds.emplace_back([this]() {
            set_current_thread_scheduler_policy();
            io_context.run();
        });
    }

    VIDEO_INFO_PRINT("VideoDriver compiled on %s at %s", __DATE__, __TIME__);
}

BackgroundService::~BackgroundService()
{
    work_guard.reset();
    io_context.stop();

    for (auto &thread : io_thds)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

asio::io_context &BackgroundService::context()
{
    return io_context;
}

// UsrQueueAsync implementation
UsrQueueAsync::UsrQueueAsync()
    : owner_(nullptr), user_data_(nullptr), running_(false), next_deliver_seq_(0), target_depth_(0)
{
}

UsrQueueAsync::~UsrQueueAsync()
{
    stop();
}

void UsrQueueAsync::set_owner(ReliableUDP *owner)
{
    owner_ = owner;
}

void UsrQueueAsync::start(RecvCallBack callback, void *user_data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
        return;

    receive_callback_ = callback;
    user_data_ = user_data;
    running_ = true;
    thread_ = std::thread(&UsrQueueAsync::worker_thread, this);
}

void UsrQueueAsync::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cond_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

bool UsrQueueAsync::enqueue(uint8_t *data, size_t size, uint32_t abs_seq)
{
    if (size > MAX_TRX_UDP_SIZE || data == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(mutex_);

    if (reorder_buf_.size() >= MAX_REORDER_BUF)
    {
        if (owner_)
            owner_->count_dropped_packet();
        return false;
    }

    // Reject frames that were already delivered (abs_seq is behind the
    // delivery cursor).  Uses unsigned comparison; only active once the
    // cursor has advanced past 0 to avoid false-positives at startup.
    if (next_deliver_seq_ != 0 && abs_seq < next_deliver_seq_)
        return false;

    auto result = reorder_buf_.emplace(abs_seq, cb_data{data, size, abs_seq});
    if (!result.second)
        return false; // duplicate seq

    // Disorder metric: how far ahead of the next expected delivery point
    // this frame is.  Measured as max(0, abs_seq - next_deliver_seq_).
    //
    // Guard: skip only the true startup bias where frame_cycle_ starts at 1,
    // making the first real abs_seq >= (1u << 16) = 65536.  The threshold
    // (1u<<16)-1 = 65535 is derived directly from that initial value:
    // unit-test sequences in [0, 65535] pass through and are recorded
    // normally; the startup real-session abs_seq (>= 65536) is suppressed.
    static constexpr uint32_t STARTUP_BIAS_THRESHOLD = (1u << 16) - 1;
    if (next_deliver_seq_ != 0 || abs_seq <= STARTUP_BIAS_THRESHOLD)
    {
        int64_t ahead = static_cast<int64_t>(abs_seq) - static_cast<int64_t>(next_deliver_seq_);
        double disorder =
            (ahead > 0) ? std::min(static_cast<double>(ahead), static_cast<double>(MAX_JITTER_DEPTH)) : 0.0;
        disorder_hist_.add(disorder);
    }

    cond_.notify_one();
    return true;
}

size_t UsrQueueAsync::target_depth() const
{
    return target_depth_.load(std::memory_order_relaxed);
}

double UsrQueueAsync::disorder_p90() const
{
    std::lock_guard<std::mutex> lk(mutex_);
    return disorder_hist_.quantile(0.90);
}

void UsrQueueAsync::worker_thread()
{
    using clock = std::chrono::steady_clock;
    auto flush_at = clock::time_point::max();

    while (true)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        if (flush_at == clock::time_point::max())
            cond_.wait(lock, [this] { return !running_ || !reorder_buf_.empty(); });
        else
            cond_.wait_until(lock, flush_at, [this] { return !running_ || !reorder_buf_.empty(); });

        if (!running_ && reorder_buf_.empty())
            break;

        bool force = (!reorder_buf_.empty() && clock::now() >= flush_at);
        bool delivered = try_deliver_locked(lock, force);

        if (delivered)
            flush_at = clock::time_point::max();
        else if (!reorder_buf_.empty() && flush_at == clock::time_point::max())
            flush_at = clock::now() + UsrQueueAsync::FLUSH_TIMEOUT;
    }
}

bool UsrQueueAsync::try_deliver_locked(std::unique_lock<std::mutex> &lock, bool force)
{
    bool any = false;
    while (!reorder_buf_.empty())
    {
        auto front_it = reorder_buf_.begin();
        bool in_order = (front_it->first == next_deliver_seq_);
        bool depth_reached = (reorder_buf_.size() > target_depth_.load(std::memory_order_relaxed));

        if (!in_order && !depth_reached && !force)
            break;

        force = false; // only one forced delivery per stall event

        cb_data item = front_it->second;

        // Align next expected seq, skipping any unrecoverable gap
        if (front_it->first != next_deliver_seq_)
            next_deliver_seq_ = front_it->first;
        next_deliver_seq_++;
        reorder_buf_.erase(front_it);

        // Update target_depth from the P90 of the disorder histogram.
        // quantile() returns NaN during bootstrap (boot_n == 0); the
        // >= 0.0 guard safely keeps target_depth_ == 0 in that case.
        //
        // Use lround (nearest integer) rather than ceil so the depth can
        // decrease: P90 < 0.5 maps to 0, 0.5-1.5 maps to 1, etc.
        double p90 = disorder_hist_.quantile(0.90);
        if (p90 >= 0.0)
        {
            size_t d = static_cast<size_t>(std::lround(p90));
            target_depth_.store(d > MAX_JITTER_DEPTH ? MAX_JITTER_DEPTH : d, std::memory_order_relaxed);
        }

        any = true;
        lock.unlock();
        receive_callback_(item.data, item.size);
        if (owner_)
            owner_->release_recv_buf(item.data);
        lock.lock();
    }
    return any;
}

// UsrQueueSync implementation
UsrQueueSync::UsrQueueSync() : owner_(nullptr), user_data_(nullptr)
{
}

void UsrQueueSync::set_owner(ReliableUDP *owner)
{
    owner_ = owner;
}

void UsrQueueSync::start(RecvCallBack callback, void *user_data)
{
    receive_callback_ = callback;
    user_data_ = user_data;
}

void UsrQueueSync::stop()
{
    receive_callback_ = nullptr;
    user_data_ = nullptr;
}

bool UsrQueueSync::enqueue(uint8_t *data, size_t size, uint32_t /*abs_seq*/)
{
    if (size > MAX_TRX_UDP_SIZE || data == nullptr)
        return false;

    receive_callback_(data, size);
    if (owner_)
        owner_->release_recv_buf(data);
    return true;
}

// ReliableUDP implementation
ReliableUDP::ReliableUDP(asio::io_context &io_context, unsigned short local_port)
    : running_(false), io_context_(io_context), strand_(io_context), receive_buffer_(new uint8_t[MAX_TRX_UDP_SIZE]),
      send_pool_(25), recv_pool_(25), recv_packets_(0), rs_(nullptr), target_uid_(0), destination_set_(false),
      frame_cycle_(1), group_cycle_(1), next_group_id_(1), next_frame_id_(1), last_frame_id_(0), last_group_id_(0),
      usr_queue_(std::make_unique<UsrQueueAsync>()), lost_packets_(0), send_bytes_(0), recv_bytes_(0),
      last_send_rate_time_(std::chrono::steady_clock::now()), last_recv_rate_time_(std::chrono::steady_clock::now()),
      last_send_rate_bytes_(0), last_recv_rate_bytes_(0), last_lost_rate_time_(std::chrono::steady_clock::now()),
      probe_timer_(io_context_), rtt_ms_(-1), offset_ms_(0),
      time_synced_(false), probe_seq_(0)
{
    // Create receive socket and bind to specific port
    recv_socket_ = std::make_unique<udp::socket>(io_context_);
    recv_socket_->open(udp::v4());

    asio::socket_base::receive_buffer_size option_recv(3 * 1024 * 1024); // 3MB
    asio::error_code ec;
    recv_socket_->set_option(option_recv, ec);
    recv_socket_->set_option(asio::socket_base::reuse_address(true), ec);

    recv_socket_->bind(udp::endpoint(udp::v4(), local_port), ec);
    if (ec)
    {
        throw std::runtime_error("ReliableUDP bind failed: " + ec.message());
    }

    // Create send socket without binding to specific port
    send_socket_ = std::make_unique<udp::socket>(io_context_);
    send_socket_->open(udp::v4());

    asio::socket_base::send_buffer_size option_send(3 * 1024 * 1024); // 3MB
    send_socket_->set_option(option_send, ec);
    send_socket_->set_option(asio::socket_base::reuse_address(true), ec);

    std::call_once(fec_init_flag, []() { fec_init(); });

    rs_ = reed_solomon_new(MAX_RS_PACKET_NUM_PER_GROUP, TRX_RS_FEC_REDUNDANCY);
    if (!rs_)
    {
        throw std::runtime_error("Failed to create Reed-Solomon encoder");
    }

    generate_uuid();
    usr_queue_->set_owner(this);

    VIDEO_INFO_PRINT("ReliableUDP initialized - receive port: %d, uuid: %u", recv_socket_->local_endpoint().port(),
                     target_uid_);
}

ReliableUDP::~ReliableUDP()
{
    stop();

    asio::error_code ec;
    recv_socket_->close(ec);

    if (ec)
    {
        VIDEO_ERROR_PRINT("Failed to close receive socket: %s", ec.message().c_str());
    }

    send_socket_->close(ec);

    if (ec)
    {
        VIDEO_ERROR_PRINT("Failed to close send socket: %s", ec.message().c_str());
    }

    reed_solomon_release(rs_);
    delete[] receive_buffer_;
}

void ReliableUDP::start()
{
    if (running_.exchange(true))
    {
        return;
    }

    start_receive();
}

bool ReliableUDP::add_destination(const std::string &address, unsigned short port)
{
    if (!running_)
    {
        VIDEO_ERROR_PRINT("ReliableUDP is not running");
        return false;
    }
    udp::endpoint endpoint;
    try
    {
        endpoint = udp::endpoint(asio::ip::make_address(address), port);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("Invalid address or port: %s", e.what());
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(target_mutex_);
        if (destination_set_)
        {
            VIDEO_ERROR_PRINT("Destination is already set");
            return false;
        }
        target_endpoint_ = endpoint;
        destination_set_ = true;
    }

    VIDEO_INFO_PRINT("Destination set to %s:%u", endpoint.address().to_string().c_str(), endpoint.port());

    asio::post(strand_, [self = shared_from_this()]() {
        self->send_probe();
        self->schedule_probe();
    });

    return true;
}

void ReliableUDP::stop()
{
    if (!running_.exchange(false))
    {
        return;
    }

    asio::error_code ec;
    recv_socket_->cancel(ec);

    if (ec)
    {
        VIDEO_ERROR_PRINT("Failed to cancel receive socket: %s", ec.message().c_str());
    }

    send_socket_->cancel(ec);

    if (ec)
    {
        VIDEO_ERROR_PRINT("Failed to cancel send socket: %s", ec.message().c_str());
    }

    probe_timer_.cancel();
    usr_queue_->stop();
}

double ReliableUDP::lost_rate()
{
    auto curr_time = std::chrono::steady_clock::now();
    auto recv_packets = recv_packets_.load(std::memory_order_relaxed);
    auto lost_packets = lost_packets_.load(std::memory_order_relaxed);

    auto real_lost_rate = 0.0;
    if (recv_packets > 0)
    {
        real_lost_rate = static_cast<double>(lost_packets) / static_cast<double>(recv_packets);
    }

    if (curr_time - last_lost_rate_time_ >= std::chrono::seconds(10))
    {
        if (real_lost_rate > 0.0)
        {
            VIDEO_INFO_PRINT("Current lost rate: %.2f%% (lost: %" PRIu64 ", recv: %" PRIu64 ")", real_lost_rate * 100.0,
                             lost_packets, recv_packets);
        }

        recv_packets_.store(0, std::memory_order_relaxed);
        lost_packets_.store(0, std::memory_order_relaxed);
        last_lost_rate_time_ = curr_time;
    }

    return real_lost_rate;
}

void ReliableUDP::count_dropped_packet()
{
    lost_packets_.fetch_add(1, std::memory_order_relaxed);
}

int64_t ReliableUDP::rtt_ms() const
{
    return rtt_ms_.load(std::memory_order_relaxed);
}

int64_t ReliableUDP::offset_ms() const
{
    return offset_ms_.load(std::memory_order_relaxed);
}

bool ReliableUDP::is_time_synced() const
{
    return time_synced_.load(std::memory_order_relaxed);
}

double ReliableUDP::send_rate()
{
    return calc_rate_bps(send_bytes_, rate_mutex_, last_send_rate_time_, last_send_rate_bytes_);
}

double ReliableUDP::recv_rate()
{
    return calc_rate_bps(recv_bytes_, rate_mutex_, last_recv_rate_time_, last_recv_rate_bytes_);
}

void ReliableUDP::start_receive()
{
    recv_socket_->async_receive_from(
        asio::buffer(receive_buffer_, MAX_TRX_UDP_SIZE), remote_endpoint_,
        strand_.wrap([self = shared_from_this()](const asio::error_code &error, size_t bytes_transferred) {
            self->handle_receive(error, bytes_transferred);
        }));
}

void ReliableUDP::handle_receive(const asio::error_code &error, size_t bytes_transferred)
{
    if (!running_)
    {
        return;
    }

    if (!error && bytes_transferred > TRX_HEADER_SIZE)
    {
        TRXUnit unit;
        std::memcpy(&unit.head, receive_buffer_, TRX_HEADER_SIZE);
        if (TRXUnit::validate(&unit.head))
        {
            const auto payload_len = bytes_transferred - TRX_HEADER_SIZE;
            const auto unit_len = static_cast<size_t>(unit.head.units_len);
            if (unit_len == 0 || unit_len > MAX_TRX_DATA_SIZE || payload_len != unit_len)
            {
                VIDEO_ERROR_PRINT("Invalid payload length: payload=%zu, unit_len=%zu, max=%zu", payload_len, unit_len,
                                  static_cast<size_t>(MAX_TRX_DATA_SIZE));
                start_receive();
                return;
            }

            recv_bytes_.fetch_add(static_cast<uint64_t>(bytes_transferred), std::memory_order_relaxed);

            unit.data = static_cast<uint8_t *>(recv_pool_.allocate(unit_len, false));
            if (unit.data)
            {
                std::memcpy(unit.data, receive_buffer_ + TRX_HEADER_SIZE, unit_len);
            }
            else
            {
                VIDEO_ERROR_PRINT("Failed to allocate receive buffer of %zu bytes", unit_len);
            }
            process_received_unit(unit);
        }
        else
        {
            VIDEO_ERROR_PRINT("TRXUnit validation failed");
        }
    }
    else if (!error && bytes_transferred == TRX_HEADER_SIZE)
    {
        TRXProbe pkt;
        std::memcpy(&pkt, receive_buffer_, sizeof(TRXProbe));
        if (pkt.magic == MAGIC_TRX_PROBE_NUMBER && (pkt.type == 0 || pkt.type == 1))
        {
            handle_probe_packet(pkt);
        }
    }

    start_receive();
}

void ReliableUDP::create_huge_rtx_group(std::vector<TRXUnit> &all_units, const uint8_t *data, size_t current_group_size,
                                        uint16_t current_group_id, uint16_t uid, uint16_t current_frame_id,
                                        uint16_t total_groups)
{
    size_t packet_size = (current_group_size + MAX_RS_PACKET_NUM_PER_GROUP - 1) / MAX_RS_PACKET_NUM_PER_GROUP;
    if (packet_size > MAX_TRX_DATA_SIZE)
    {
        VIDEO_ERROR_PRINT("Packet size %zu exceeds MAX_DATA_SIZE %zu", packet_size, MAX_TRX_DATA_SIZE);
        return;
    }

    unsigned char *data_blocks[MAX_RS_PACKET_NUM_PER_GROUP];
    unsigned char *fec_blocks[TRX_RS_FEC_REDUNDANCY];

    all_units.reserve(MAX_RS_PACKET_NUM_PER_GROUP + TRX_RS_FEC_REDUNDANCY);

    size_t group_offset = 0;
    for (size_t i = 0; i < MAX_RS_PACKET_NUM_PER_GROUP; ++i)
    {
        TRXUnit unit;
        unit.head.group_seq = current_group_id;
        unit.head.units_idx = static_cast<uint8_t>(i);
        unit.head.units_num = static_cast<uint8_t>(MAX_RS_PACKET_NUM_PER_GROUP + TRX_RS_FEC_REDUNDANCY);
        unit.head.group_len = static_cast<uint16_t>(current_group_size);
        unit.head.frame_seq = current_frame_id;
        unit.head.group_num = static_cast<uint8_t>(total_groups);
        unit.head.units_len = static_cast<uint16_t>(packet_size);
        unit.head.conn_uuid = uid;
        unit.head.check_sum = TRXUnit::adler_8(&unit.head);
        unit.data = static_cast<uint8_t *>(send_pool_.allocate(packet_size));

        size_t current_size = 0;
        if (group_offset < current_group_size)
        {
            current_size = std::min(packet_size, current_group_size - group_offset);
        }

        memcpy(unit.data, data + group_offset, current_size);
        if (current_size != packet_size)
        {
            memset(unit.data + current_size, 0, packet_size - current_size);
        }

        group_offset += current_size;

        data_blocks[i] = unit.data;
        all_units.push_back(unit);
    }

    for (size_t i = 0; i < TRX_RS_FEC_REDUNDANCY; ++i)
    {
        TRXUnit fec_unit;
        fec_unit.head.group_seq = current_group_id;
        fec_unit.head.units_idx = static_cast<uint8_t>(MAX_RS_PACKET_NUM_PER_GROUP + i);
        fec_unit.head.units_num = static_cast<uint8_t>(MAX_RS_PACKET_NUM_PER_GROUP + TRX_RS_FEC_REDUNDANCY);
        fec_unit.head.group_len = static_cast<uint16_t>(current_group_size);
        fec_unit.head.frame_seq = current_frame_id;
        fec_unit.head.group_num = static_cast<uint8_t>(total_groups);
        fec_unit.head.units_len = static_cast<uint16_t>(packet_size);
        fec_unit.head.conn_uuid = uid;
        fec_unit.head.check_sum = TRXUnit::adler_8(&fec_unit.head);

        fec_unit.data = static_cast<uint8_t *>(send_pool_.allocate(packet_size));
        memset(fec_unit.data, 0, packet_size);

        fec_blocks[i] = fec_unit.data;
        all_units.push_back(std::move(fec_unit));
    }

    int encode_result = reed_solomon_encode(rs_, data_blocks, fec_blocks, static_cast<int>(packet_size));

    if (encode_result != 0)
    {
        VIDEO_ERROR_PRINT("Reed-Solomon encoding failed for group %u", current_group_id);
        for (auto &unit : all_units)
        {
            send_pool_.deallocate(unit.data);
        }
        all_units.clear();
    }
}

void ReliableUDP::create_small_rtx_group(std::vector<TRXUnit> &all_units, const uint8_t *data,
                                         size_t current_group_size, uint16_t current_group_id, uint16_t uid,
                                         uint16_t current_frame_id, uint16_t total_groups)
{
    TRXUnit unit;
    unit.head.group_seq = current_group_id;
    unit.head.units_idx = 0;
    unit.head.units_num = 2;
    unit.head.group_len = static_cast<uint16_t>(current_group_size);
    unit.head.frame_seq = current_frame_id;
    unit.head.group_num = total_groups;
    unit.head.units_len = static_cast<uint16_t>(current_group_size);
    unit.head.conn_uuid = uid;
    unit.head.check_sum = TRXUnit::adler_8(&unit.head);

    unit.data = static_cast<uint8_t *>(send_pool_.allocate(current_group_size));
    memcpy(unit.data, data, current_group_size);

    auto redundant_unit = unit;
    redundant_unit.head.units_idx = 1;
    redundant_unit.head.check_sum = TRXUnit::adler_8(&redundant_unit.head);
    redundant_unit.data = static_cast<uint8_t *>(send_pool_.allocate(current_group_size));
    memcpy(redundant_unit.data, data, current_group_size);

    all_units.push_back(unit);
    all_units.push_back(redundant_unit);
}

void ReliableUDP::create_no_fec_group(std::vector<TRXUnit> &all_units, const uint8_t *data, size_t current_group_size,
                                      uint16_t current_group_id, uint16_t uid, uint16_t current_frame_id,
                                      uint16_t total_groups)
{
    TRXUnit unit;
    unit.head.group_seq = current_group_id;
    unit.head.units_idx = 0;
    unit.head.units_num = 1;
    unit.head.group_len = static_cast<uint16_t>(current_group_size);
    unit.head.frame_seq = current_frame_id;
    unit.head.group_num = total_groups;
    unit.head.units_len = static_cast<uint16_t>(current_group_size);
    unit.head.conn_uuid = uid;
    unit.head.check_sum = TRXUnit::adler_8(&unit.head);

    unit.data = static_cast<uint8_t *>(send_pool_.allocate(current_group_size));
    memcpy(unit.data, data, current_group_size);

    all_units.push_back(unit);
}

TRXFECMode ReliableUDP::resolve_fec_mode(size_t packet_size) const
{
    return packet_size > MAX_TRX_DATA_SIZE ? TRXFECMode::RS : TRXFECMode::XOR;
}

std::vector<TRXUnit> ReliableUDP::create_trx_units(const uint8_t *data, size_t size)
{
    const TRXFECMode fec_mode = resolve_fec_mode(size);

    size_t max_group_data_size;
    switch (fec_mode)
    {
    case TRXFECMode::RS:
        max_group_data_size = MAX_RS_PACKET_NUM_PER_GROUP * MAX_TRX_DATA_SIZE;
        break;
    case TRXFECMode::XOR:
    case TRXFECMode::None:
    default:
        max_group_data_size = MAX_TRX_DATA_SIZE;
        break;
    }

    const size_t total_groups = (size + max_group_data_size - 1) / max_group_data_size;

    std::vector<TRXUnit> all_units;

    size_t offset = 0;
    uint16_t current_frame_id = next_frame_id_++;
    uint16_t current_group_id = (next_group_id_ >= 0xffffu - MAX_GROUP_NUM_PER_FRAME) ? 0 : next_group_id_;

    for (size_t group_idx = 0; group_idx < total_groups; ++group_idx)
    {
        size_t remaining_size = size - offset;
        size_t current_group_size = std::min(remaining_size, max_group_data_size);

        switch (fec_mode)
        {
        case TRXFECMode::RS:
            create_huge_rtx_group(all_units, data + offset, current_group_size, current_group_id, target_uid_,
                                  static_cast<uint16_t>(current_frame_id), static_cast<uint16_t>(total_groups));
            break;
        case TRXFECMode::XOR:
            create_small_rtx_group(all_units, data + offset, current_group_size, current_group_id, target_uid_,
                                   static_cast<uint16_t>(current_frame_id), static_cast<uint16_t>(total_groups));
            break;
        case TRXFECMode::None:
        default:
            create_no_fec_group(all_units, data + offset, current_group_size, current_group_id, target_uid_,
                                static_cast<uint16_t>(current_frame_id), static_cast<uint16_t>(total_groups));
            break;
        }

        offset += current_group_size;
        current_group_id++;
    }
    next_group_id_ = current_group_id;
    return all_units;
}

void ReliableUDP::process_received_unit(const TRXUnit &unit)
{
    auto seq = unit.head.group_seq;
    auto uid = static_cast<uint16_t>(unit.head.conn_uuid);
    auto idx = static_cast<size_t>(unit.head.units_idx);
    auto now = std::chrono::steady_clock::now();

    if (last_group_id_ > 3 * 0xffffu / 4 && seq < 0xffffu / 4)
    {
        group_cycle_++;
    }
    last_group_id_ = seq;

    if (unit.data)
    {
        auto it = std::find_if(receive_groups_.begin(), receive_groups_.end(), [seq, uid, this](const TRXGroup &group) {
            return group.trxuint_guuid == uid && group.trxunit_group == seq && group.trxunit_cycle == group_cycle_;
        });

        if (it == receive_groups_.end())
        {
            TRXGroup new_group;
            new_group.trxunit_cycle = group_cycle_;
            new_group.trxunit_group = seq;
            new_group.trxuint_guuid = uid;
            new_group.trxunit_recvd = 0;
            new_group.timestamp = now;
            receive_groups_.push_front(new_group);
            it = receive_groups_.begin();
            recv_packets_ += unit.head.units_num;
        }

        auto units_num = it->units.empty() ? static_cast<uint8_t>(unit.head.units_num)
                                           : static_cast<uint8_t>(it->units.front().head.units_num);

        if (it->has_received(idx))
        {
            recv_pool_.deallocate(unit.data);
            return;
        }

        if (!it->units.empty() && units_num != static_cast<uint8_t>(unit.head.units_num))
        {
            VIDEO_ERROR_PRINT("Inconsistent units_num for group (%u,%u)", it->trxunit_cycle, it->trxunit_group);
            recv_pool_.deallocate(unit.data);
            return;
        }

        it->set_received(idx);

        auto layout = resolve_fec_layout(units_num);
        auto data_packets_count = layout.data_packets_count;

        auto it_recv_count = it->received_cnt();
        if (it_recv_count < data_packets_count)
        {
            it->units.push_back(unit);
        }
        else if (it_recv_count == data_packets_count)
        {
            it->units.push_back(unit);
            auto sentinel = it->units.front();
            sentinel.data = nullptr;
            if (layout.mode != TRXFECMode::RS)
            {
                assemble_complete_message(unit.head.frame_seq, unit.head.group_num, unit.head.group_seq, unit.data,
                                          unit.head.group_len);
            }
            else
            {
                auto units_copy = std::move(it->units);
                try_recover_group(units_copy, data_packets_count);
            }
            it->units.clear();
            it->units.push_back(sentinel);
        }
        else if (it_recv_count == unit.head.units_num)
        {
            recv_pool_.deallocate(unit.data);
            receive_groups_.erase(it);
        }
        else
        {
            recv_pool_.deallocate(unit.data);
        }

        while (!receive_groups_.empty() && now - receive_groups_.back().timestamp > std::chrono::seconds(3))
        {
            auto &oldest_group = receive_groups_.back();
            auto recv_cnt = oldest_group.received_cnt();
            auto oldest_units_num = oldest_group.units.empty()
                                        ? static_cast<uint8_t>(0)
                                        : static_cast<uint8_t>(oldest_group.units.front().head.units_num);
            auto oldest_layout = resolve_fec_layout(oldest_units_num);

            lost_packets_ +=
                static_cast<uint64_t>(oldest_layout.data_packets_count + oldest_layout.fec_packets_count - recv_cnt);

            for (auto &u : oldest_group.units)
            {
                recv_pool_.deallocate(u.data);
            }

            receive_groups_.pop_back();
        }
    }
}

void ReliableUDP::try_recover_group(std::vector<TRXUnit> &units, uint8_t data_packets_count)
{
    if (units.empty() || !rs_)
    {
        return;
    }

    std::sort(units.begin(), units.end(),
              [](const TRXUnit &a, const TRXUnit &b) { return a.head.units_idx < b.head.units_idx; });

    uint16_t group_len = units[0].head.group_len;
    size_t packet_size = units[0].head.units_len;
    unsigned char *data_blocks[MAX_RS_PACKET_NUM_PER_GROUP]{};
    for (const auto &unit : units)
    {
        if (unit.head.units_idx < MAX_RS_PACKET_NUM_PER_GROUP)
        {
            data_blocks[unit.head.units_idx] = unit.data;
        }
    }

    bool has_missing_data = std::any_of(data_blocks, data_blocks + data_packets_count,
                                        [](const unsigned char *ptr) { return ptr == nullptr; });

    std::vector<uint8_t *> temp_blocks;
    auto cleanup = [&]() {
        for (auto *temp_block : temp_blocks)
        {
            recv_pool_.deallocate(temp_block);
        }
        for (auto &unit : units)
        {
            recv_pool_.deallocate(unit.data);
        }
    };

    if (has_missing_data)
    {
        unsigned char *fec_blocks[TRX_RS_FEC_REDUNDANCY]{};
        unsigned int fec_block_pos[TRX_RS_FEC_REDUNDANCY]{};
        unsigned int erasures[TRX_RS_FEC_REDUNDANCY];
        unsigned int erasure_count = 0;

        size_t fec_idx = 0;
        for (const auto &unit : units)
        {
            if (unit.head.units_idx >= MAX_RS_PACKET_NUM_PER_GROUP &&
                unit.head.units_idx < (MAX_RS_PACKET_NUM_PER_GROUP + TRX_RS_FEC_REDUNDANCY))
            {
                fec_blocks[fec_idx] = unit.data;
                fec_block_pos[fec_idx] = unit.head.units_idx - MAX_RS_PACKET_NUM_PER_GROUP;
                fec_idx++;
            }
        }

        for (size_t i = 0; i < data_packets_count; ++i)
        {
            if (data_blocks[i] == nullptr)
            {
                erasures[erasure_count++] = static_cast<unsigned int>(i);
                data_blocks[i] = static_cast<uint8_t *>(recv_pool_.allocate(MAX_TRX_DATA_SIZE));
                memset(data_blocks[i], 0, packet_size);
                temp_blocks.push_back(data_blocks[i]);
            }
        }

        if (erasure_count > TRX_RS_FEC_REDUNDANCY)
        {
            VIDEO_ERROR_PRINT("Too many missing packets (%d), cannot recover", erasure_count);
            cleanup();
            return;
        }

        if (reed_solomon_decode(rs_, data_blocks, static_cast<int>(packet_size), fec_blocks, fec_block_pos, erasures,
                                erasure_count) != 0)
        {
            VIDEO_ERROR_PRINT("Reed-Solomon decoding failed");
            cleanup();
            return;
        }
    }

    auto *assembled = static_cast<uint8_t *>(recv_pool_.allocate(group_len));
    if (!assembled)
    {
        cleanup();
        return;
    }

    size_t offset = 0;
    for (size_t i = 0; i < data_packets_count; ++i)
    {
        size_t current_len = (i < data_packets_count - 1u) ? packet_size : (group_len - i * packet_size);
        std::memcpy(assembled + offset, data_blocks[i], current_len);
        offset += current_len;
    }
    cleanup();

    assemble_complete_message(units[0].head.frame_seq, units[0].head.group_num, units[0].head.group_seq, assembled,
                              group_len);
}

void ReliableUDP::assemble_complete_message(uint16_t frame_seq, uint16_t group_num, uint16_t group_seq, uint8_t *data,
                                            size_t size)
{
    if (last_frame_id_ > 3 * 0xffffu / 4 && frame_seq < 0xffffu / 4)
    {
        frame_cycle_++;
        VIDEO_DEBUG_PRINT("Frame wrap around detected: %u -> %u", last_frame_id_, frame_seq);
    }
    last_frame_id_ = frame_seq;

    auto it = std::find_if(receive_frames_.begin(), receive_frames_.end(), [this, frame_seq](const TRXFrame &f) {
        return f.frame_seq == frame_seq && f.frame_cyc == frame_cycle_;
    });

    if (it == receive_frames_.end())
    {
        receive_frames_.emplace_front(frame_cycle_, frame_seq, group_num, group_seq, data, size);
        it = receive_frames_.begin();
    }
    else
    {
        it->recvd_num++;
        uint64_t packed_info = (static_cast<uint64_t>(group_seq) << 32) | static_cast<uint32_t>(size);
        it->fragments.emplace_back(packed_info, data);
    }

    if (it->recvd_num == it->group_num)
    {
        std::sort(it->fragments.begin(), it->fragments.end(),
                  [](const TRXFrame::Fragment &a, const TRXFrame::Fragment &b) {
                      return TRXFrame::extract_group_seq(a.first) < TRXFrame::extract_group_seq(b.first);
                  });

        size_t total_length = 0;
        for (const auto &frag : it->fragments)
        {
            total_length += TRXFrame::extract_size(frag.first);
        }

        auto *complete_message = static_cast<uint8_t *>(recv_pool_.allocate(total_length));
        size_t offset = 0;

        for (const auto &frag : it->fragments)
        {
            uint32_t frag_size = TRXFrame::extract_size(frag.first);
            std::memcpy(complete_message + offset, frag.second, frag_size);
            offset += frag_size;
            recv_pool_.deallocate(frag.second);
        }

        uint32_t abs_seq = (static_cast<uint32_t>(frame_cycle_) << 16) | frame_seq;
        if (!usr_queue_->enqueue(complete_message, total_length, abs_seq))
        {
            VIDEO_ERROR_PRINT("Failed to enqueue received message");
            recv_pool_.deallocate(complete_message); // queue refused — release here
        }
        receive_frames_.erase(it);
    }

    if (receive_frames_.size() > MAX_TRX_RECEIVE_FRAMES)
    {
        auto &oldest_frame = receive_frames_.back();
        VIDEO_DEBUG_PRINT("Cleaning up frame (%u,%u) with %zu fragments, expected %u", oldest_frame.frame_cyc,
                          oldest_frame.frame_seq, oldest_frame.fragments.size(), oldest_frame.group_num);
        for (const auto &frag : oldest_frame.fragments)
        {
            recv_pool_.deallocate(frag.second);
        }
        receive_frames_.pop_back();
    }
}

void ReliableUDP::release_recv_buf(uint8_t *p)
{
    recv_pool_.deallocate(p);
}

void ReliableUDP::generate_uuid()
{
    auto current_ms = std::chrono::steady_clock::now().time_since_epoch();
    auto ms_count = std::chrono::duration_cast<std::chrono::milliseconds>(current_ms).count();
    target_uid_ = static_cast<uint16_t>(ms_count % 4093);
}

bool ReliableUDP::send(const uint8_t *data, size_t size)
{
    if (!running_)
    {
        return false;
    }

    if (!data || size == 0 || size > MAX_TRX_UDP_SIZE)
    {
        VIDEO_ERROR_PRINT("Invalid data or size for sending");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(target_mutex_);
        if (!destination_set_)
        {
            VIDEO_ERROR_PRINT("No destination set. need negotiate first");
            return false;
        }
    }

    auto units = create_trx_units(data, size);
    if (units.empty())
    {
        VIDEO_ERROR_PRINT("Failed to create TRX units for payload size %zu", size);
        return false;
    }

    try
    {
        for (auto &unit : units)
        {
            std::array<asio::const_buffer, 2> buffers = {asio::buffer(&unit.head, TRX_HEADER_SIZE),
                                                         asio::buffer(unit.data, unit.head.units_len)};
            send_socket_->send_to(buffers, target_endpoint_);
            send_bytes_.fetch_add(static_cast<uint64_t>(TRX_HEADER_SIZE + unit.head.units_len),
                                  std::memory_order_relaxed);
            send_pool_.deallocate(unit.data);
            unit.data = nullptr;
        }
        return true;
    }
    catch (const std::exception &e)
    {
        for (auto &unit : units)
        {
            if (unit.data)
            {
                send_pool_.deallocate(unit.data);
                unit.data = nullptr;
            }
        }
        VIDEO_ERROR_PRINT("Error occurred while sending data: %s", e.what());
        return false;
    }
}

uint32_t ReliableUDP::get_time_ms()
{
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void ReliableUDP::send_probe()
{
    udp::endpoint target;
    {
        std::lock_guard<std::mutex> lock(target_mutex_);
        if (!destination_set_)
        {
            return;
        }
        target = target_endpoint_;
    }

    TRXProbe pkt{};
    pkt.magic = MAGIC_TRX_PROBE_NUMBER;
    pkt.type = 0;
    pkt.seq = probe_seq_++;
    pkt.t1_ms = get_time_ms();

    auto buf = std::make_shared<TRXProbe>(pkt);
    send_socket_->async_send_to(asio::buffer(buf.get(), sizeof(TRXProbe)), target,
                                [buf](const asio::error_code &ec, size_t) {
                                    if (ec)
                                    {
                                        VIDEO_ERROR_PRINT("[Probe] Failed to send ping: %s", ec.message().c_str());
                                    }
                                });
}

void ReliableUDP::schedule_probe()
{
    probe_timer_.expires_after(std::chrono::seconds(5));
    probe_timer_.async_wait(strand_.wrap([self = shared_from_this()](const asio::error_code &ec) {
        if (!ec)
        {
            self->send_probe();
            self->schedule_probe();
        }
    }));
}

void ReliableUDP::handle_probe_packet(const TRXProbe &pkt)
{
    if (pkt.type == 0)
    {
        // Ping received: reply with a pong.
        udp::endpoint target;
        {
            std::lock_guard<std::mutex> lock(target_mutex_);
            if (!destination_set_)
            {
                return;
            }
            target = target_endpoint_;
        }

        TRXProbe pong{};
        pong.magic = MAGIC_TRX_PROBE_NUMBER;
        pong.type = 1;
        pong.seq = pkt.seq;
        pong.t1_ms = pkt.t1_ms;
        pong.t2_delta_ms = static_cast<int32_t>(get_time_ms() - pkt.t1_ms);

        auto buf = std::make_shared<TRXProbe>(pong);
        send_socket_->async_send_to(asio::buffer(buf.get(), sizeof(TRXProbe)), target,
                                    [buf](const asio::error_code &ec, size_t) {
                                        if (ec)
                                        {
                                            VIDEO_ERROR_PRINT("[Probe] Failed to send pong: %s", ec.message().c_str());
                                        }
                                    });
    }
    else if (pkt.type == 1)
    {
        // Pong received: compute RTT and clock offset.
        // Simplified NTP (T3 ~= T2 because pong is sent immediately):
        //   RTT    = T4 - T1
        //   offset = T2_delta - RTT/2
        uint32_t t4_ms = get_time_ms();
        int32_t rtt = static_cast<int32_t>(t4_ms - pkt.t1_ms);
        int32_t offset = pkt.t2_delta_ms - rtt / 2;

        if (rtt >= 0)
        {
            rtt_ms_.store(static_cast<int64_t>(rtt), std::memory_order_relaxed);
            offset_ms_.store(static_cast<int64_t>(offset), std::memory_order_relaxed);
            time_synced_.store(true, std::memory_order_relaxed);
        }
    }
}
