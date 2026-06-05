#include "ReliableUDP.h"
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <iomanip>
#include <pthread.h>
#include <sstream>
#include <tuple>

using asio::ip::udp;

constexpr static auto MIN_FRAME_INTERVAL_MS = 1.0;
constexpr static auto TRX_RS_FEC_GROUP_UNIT_NUMS = MAX_RS_PACKET_NUM_PER_GROUP + TRX_RS_FEC_REDUNDANCY;
constexpr static auto TRX_XOR_FEC_GROUP_UNIT_NUMS = MAX_XOR_PACKET_NUM_PER_GROUP + TRX_XOR_FEC_REDUNDANCY;

static std::once_flag fec_init_flag;

template <typename T> static T clamp_value(const T value, const T low, const T high)
{
    return std::max(low, std::min(value, high));
}

static inline uint32_t get_time_ms()
{
    using namespace std::chrono;
    return static_cast<uint32_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

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
    : frame_pool_(64), running_(false), primed_(false), expected_seq_(0), gap_active_(false), has_arrival_ref_(false),
      last_rate_seq_(0), has_highest_arrival_seq_(false), highest_arrival_seq_(0), avg_interval_ms_(0.0),
      jitter_ms_(0.0), avg_depth_frames_(0.0), disorder_depth_frames_(0.0), disorder_guard_depth_frames_(0.0)
{
    sanitize_tuning_locked();
}

UsrQueueAsync::~UsrQueueAsync()
{
    stop();
}

void UsrQueueAsync::sanitize_tuning_locked()
{
    tuning_.target_depth = std::max<size_t>(1, tuning_.target_depth);
    tuning_.startup_depth = std::max<size_t>(1, tuning_.startup_depth);
    tuning_.max_buffered_frames = std::max<size_t>(2, tuning_.max_buffered_frames);

    tuning_.target_depth = std::min(tuning_.target_depth, tuning_.max_buffered_frames);
    tuning_.startup_depth = std::min(tuning_.startup_depth, tuning_.max_buffered_frames);

    tuning_.stale_timeout_ms = std::max(tuning_.stale_timeout_ms, MIN_FRAME_INTERVAL_MS);
    tuning_.default_frame_interval_ms = std::max(tuning_.default_frame_interval_ms, MIN_FRAME_INTERVAL_MS);
    tuning_.depth_feedback_gain = clamp_value(tuning_.depth_feedback_gain, 0.0, 0.5);
    tuning_.min_pacing_factor = clamp_value(tuning_.min_pacing_factor, 0.1, 1.0);
    tuning_.max_pacing_factor = std::max(tuning_.max_pacing_factor, 1.0);
    if (tuning_.max_pacing_factor < tuning_.min_pacing_factor)
        tuning_.max_pacing_factor = tuning_.min_pacing_factor;
}

void UsrQueueAsync::reset_state_locked()
{
    buffered_frames_.clear();
    primed_ = false;
    expected_seq_ = 0;
    next_delivery_time_ = ClockTP{};
    gap_active_ = false;
    gap_start_time_ = ClockTP{};

    has_arrival_ref_ = false;
    last_rate_seq_ = 0;
    last_rate_time_ = ClockTP{};
    has_highest_arrival_seq_ = false;
    highest_arrival_seq_ = 0;
    avg_interval_ms_ = 0.0;
    jitter_ms_ = 0.0;
    avg_depth_frames_ = 0.0;
    disorder_depth_frames_ = 0.0;
    disorder_guard_depth_frames_ = 0.0;
    counters_ = Counters{};
}

void UsrQueueAsync::start(RecvCallBack callback, void *user_data)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
        return;

    receive_callback_ = callback;
    (void)user_data;
    sanitize_tuning_locked();
    reset_state_locked();
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

void UsrQueueAsync::update_depth_estimate_locked(double depth)
{
    constexpr double ALPHA_DEPTH = 0.05;
    if (avg_depth_frames_ <= 0.0)
    {
        avg_depth_frames_ = depth;
        return;
    }

    avg_depth_frames_ += ALPHA_DEPTH * (depth - avg_depth_frames_);
}

void UsrQueueAsync::update_estimators_locked(uint32_t abs_seq, ClockTP arrival)
{
    if (!has_arrival_ref_)
    {
        has_arrival_ref_ = true;
        last_rate_seq_ = abs_seq;
        last_rate_time_ = arrival;
        return;
    }

    const int64_t seq_delta = static_cast<int64_t>(abs_seq) - static_cast<int64_t>(last_rate_seq_);
    if (seq_delta <= 0)
        return;

    if (seq_delta > 0 && seq_delta <= static_cast<int64_t>(tuning_.max_buffered_frames))
    {
        const double diff_ms = std::chrono::duration<double, std::milli>(arrival - last_rate_time_).count();
        if (diff_ms > 0.0 && diff_ms < 1000.0)
        {
            constexpr double ALPHA_RATE = 0.02;
            constexpr double ALPHA_JITTER = 0.10;
            const double sample_interval_ms = diff_ms / static_cast<double>(seq_delta);
            if (avg_interval_ms_ < MIN_FRAME_INTERVAL_MS)
            {
                avg_interval_ms_ = sample_interval_ms;
                jitter_ms_ = 0.0;
            }
            else
            {
                const double deviation = std::abs(sample_interval_ms - avg_interval_ms_);
                avg_interval_ms_ += ALPHA_RATE * (sample_interval_ms - avg_interval_ms_);
                jitter_ms_ += ALPHA_JITTER * (deviation - jitter_ms_);
            }
        }
    }

    last_rate_seq_ = abs_seq;
    last_rate_time_ = arrival;
}

void UsrQueueAsync::note_disorder_locked(uint32_t abs_seq)
{
    constexpr double ALPHA_DISORDER = 0.10;
    constexpr double DECAY_DISORDER = 0.02;
    constexpr double DECAY_DISORDER_GUARD = 0.01;

    double sample = 0.0;
    if (!has_highest_arrival_seq_)
    {
        has_highest_arrival_seq_ = true;
        highest_arrival_seq_ = abs_seq;
        return;
    }

    const int64_t seq_delta = static_cast<int64_t>(abs_seq) - static_cast<int64_t>(highest_arrival_seq_);
    if (seq_delta < 0 && -seq_delta <= static_cast<int64_t>(tuning_.max_buffered_frames))
    {
        sample = static_cast<double>(-seq_delta);
        ++counters_.reorder;
        counters_.max_disorder_depth = std::max(counters_.max_disorder_depth, static_cast<uint32_t>(sample));
    }
    else if (seq_delta > 0)
    {
        highest_arrival_seq_ = abs_seq;
    }

    if (sample > 0.0)
    {
        disorder_depth_frames_ += ALPHA_DISORDER * (sample - disorder_depth_frames_);
        disorder_guard_depth_frames_ = std::max(disorder_guard_depth_frames_, sample);
    }
    else
    {
        disorder_depth_frames_ += DECAY_DISORDER * (0.0 - disorder_depth_frames_);
        disorder_guard_depth_frames_ += DECAY_DISORDER_GUARD * (0.0 - disorder_guard_depth_frames_);
    }
}

double UsrQueueAsync::bootstrap_interval_locked() const
{
    if (avg_interval_ms_ >= MIN_FRAME_INTERVAL_MS)
        return avg_interval_ms_;

    if (buffered_frames_.size() >= 2)
    {
        const auto &first = buffered_frames_.front();
        const auto &last = buffered_frames_.back();
        const double span_ms = std::chrono::duration<double, std::milli>(last.arrival - first.arrival).count() /
                               static_cast<double>(buffered_frames_.size() - 1);
        if (span_ms >= MIN_FRAME_INTERVAL_MS)
            return span_ms;
    }

    return tuning_.default_frame_interval_ms;
}

double UsrQueueAsync::compute_delivery_interval_locked() const
{
    const double base_interval_ms = std::max(MIN_FRAME_INTERVAL_MS, bootstrap_interval_locked());
    const double smoothed_depth = 0.5 * (avg_depth_frames_ + static_cast<double>(buffered_frames_.size()));
    const double depth_error = smoothed_depth - static_cast<double>(tuning_.target_depth);
    const double correction = clamp_value(1.0 - tuning_.depth_feedback_gain * depth_error, tuning_.min_pacing_factor,
                                          tuning_.max_pacing_factor);
    return std::max(MIN_FRAME_INTERVAL_MS, base_interval_ms * correction);
}

double UsrQueueAsync::compute_gap_wait_ms_locked() const
{
    const double base_interval_ms = std::max(tuning_.default_frame_interval_ms, bootstrap_interval_locked());
    const double jitter_guard_ms = std::max(base_interval_ms, jitter_ms_ * 2.0);
    const double disorder_guard_frames = std::max(disorder_depth_frames_, disorder_guard_depth_frames_);
    const double disorder_guard_ms = std::max(1.0, disorder_guard_frames) * base_interval_ms;
    const double wait_ms =
        base_interval_ms * static_cast<double>(tuning_.target_depth + 1) + jitter_guard_ms + disorder_guard_ms;
    return clamp_value(wait_ms, base_interval_ms, tuning_.stale_timeout_ms);
}

void UsrQueueAsync::drop_frame_locked(std::list<BufferedFrame>::iterator it)
{
    uint8_t *data = it->data;
    buffered_frames_.erase(it);
    release_frame_data(data);
}

void UsrQueueAsync::release_frame_data(uint8_t *data)
{
    frame_pool_.deallocate(data);
}

void UsrQueueAsync::purge_stale_locked(ClockTP now)
{
    const auto cutoff = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double, std::milli>(tuning_.stale_timeout_ms));
    for (auto it = buffered_frames_.begin(); it != buffered_frames_.end();)
    {
        if (it->arrival >= cutoff)
        {
            ++it;
            continue;
        }

        VIDEO_DEBUG_PRINT("[JitterBuf] stale drop seq=%u", it->seq);
        ++counters_.drop;
        ++counters_.stale;
        auto to_drop = it++;
        drop_frame_locked(to_drop);
    }
}

bool UsrQueueAsync::enqueue(uint8_t *data, size_t size, uint32_t abs_seq)
{
    if (size > MAX_TRX_UDP_SIZE || data == nullptr)
        return false;

    const ClockTP arrival = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_)
        return false;

    ++counters_.recv;

    if (primed_ && abs_seq < expected_seq_)
    {
        ++counters_.late;
        return false;
    }

    auto it = buffered_frames_.begin();
    while (it != buffered_frames_.end() && it->seq < abs_seq)
        ++it;
    if (it != buffered_frames_.end() && it->seq == abs_seq)
    {
        ++counters_.duplicate;
        return false;
    }

    auto *queued_data = static_cast<uint8_t *>(frame_pool_.allocate(size));
    if (!queued_data)
        return false;

    std::memcpy(queued_data, data, size);
    buffered_frames_.insert(it, {abs_seq, queued_data, size, arrival});
    update_estimators_locked(abs_seq, arrival);
    note_disorder_locked(abs_seq);
    update_depth_estimate_locked(static_cast<double>(buffered_frames_.size()));

    purge_stale_locked(arrival);

    while (buffered_frames_.size() > tuning_.max_buffered_frames)
    {
        auto tail = std::prev(buffered_frames_.end());
        VIDEO_DEBUG_PRINT("[JitterBuf] overflow drop seq=%u", tail->seq);
        ++counters_.drop;
        ++counters_.overflow;
        drop_frame_locked(tail);
    }

    cond_.notify_one();
    return true;
}

std::string UsrQueueAsync::stats_text() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const double interval_ms = std::max(MIN_FRAME_INTERVAL_MS, bootstrap_interval_locked());

    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << "q_avg_fi=" << interval_ms << "ms, q_jitter=" << jitter_ms_ << "ms/"
       << (jitter_ms_ / interval_ms) << "f, q_dis=" << disorder_depth_frames_ << "f/" << counters_.max_disorder_depth
       << ", q_depth=" << buffered_frames_.size() << '/' << avg_depth_frames_ << '/' << tuning_.target_depth
       << ", q_recv=" << counters_.recv << ", q_dlv=" << counters_.deliver << ", q_skip=" << counters_.skip
       << ", q_drop=" << counters_.drop << ", q_dup=" << counters_.duplicate << ", q_late=" << counters_.late
       << ", q_reorder=" << counters_.reorder << ", q_stale=" << counters_.stale << ", q_ovf=" << counters_.overflow;
    return os.str();
}

void UsrQueueAsync::worker_thread()
{
    set_current_thread_scheduler_policy();

    while (true)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        const auto now = std::chrono::steady_clock::now();
        purge_stale_locked(now);

        if (!running_)
        {
            drain_locked(lock);
            break;
        }

        if (buffered_frames_.empty())
        {
            gap_active_ = false;
            cond_.wait(lock, [this] { return !running_ || !buffered_frames_.empty(); });
            continue;
        }

        if (!primed_)
        {
            if (buffered_frames_.size() < tuning_.startup_depth)
            {
                cond_.wait(lock, [this] { return !running_ || buffered_frames_.size() >= tuning_.startup_depth; });
                continue;
            }

            primed_ = true;
            expected_seq_ = buffered_frames_.front().seq;
            next_delivery_time_ = now;
            gap_active_ = false;
        }

        if (std::chrono::steady_clock::now() < next_delivery_time_)
        {
            cond_.wait_until(lock, next_delivery_time_);
            continue;
        }

        if (buffered_frames_.front().seq == expected_seq_)
        {
            deliver_one_locked(lock);
            continue;
        }

        if (buffered_frames_.front().seq < expected_seq_)
        {
            ++counters_.drop;
            auto stale_front = buffered_frames_.begin();
            drop_frame_locked(stale_front);
            continue;
        }

        if (!gap_active_)
        {
            gap_active_ = true;
            gap_start_time_ = std::chrono::steady_clock::now();
        }

        const auto gap_deadline =
            gap_start_time_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double, std::milli>(compute_gap_wait_ms_locked()));
        const size_t pressure_depth =
            tuning_.target_depth + static_cast<size_t>(std::ceil(disorder_guard_depth_frames_));
        const bool depth_pressure = buffered_frames_.size() > pressure_depth;
        const auto gap_now = std::chrono::steady_clock::now();
        if (depth_pressure || gap_now >= gap_deadline)
        {
            skip_gap_locked(buffered_frames_.front().seq);
            continue;
        }

        cond_.wait_until(lock, gap_deadline, [this, pressure_depth] {
            return !running_ || buffered_frames_.empty() || buffered_frames_.front().seq <= expected_seq_ ||
                   buffered_frames_.size() > pressure_depth;
        });
    }
}

void UsrQueueAsync::deliver_one_locked(std::unique_lock<std::mutex> &lock)
{
    if (buffered_frames_.empty())
        return;

    auto frame = buffered_frames_.front();
    buffered_frames_.pop_front();

    const ClockTP now = std::chrono::steady_clock::now();
    expected_seq_ = frame.seq + 1;
    gap_active_ = false;
    update_depth_estimate_locked(static_cast<double>(buffered_frames_.size()));
    next_delivery_time_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double, std::milli>(compute_delivery_interval_locked()));

    ++counters_.deliver;

    lock.unlock();
    try
    {
        if (receive_callback_)
            receive_callback_(frame.data, frame.size);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("[JitterBuf] receive callback threw exception: %s", e.what());
    }
    catch (...)
    {
        VIDEO_ERROR_PRINT("[JitterBuf] receive callback threw unknown exception");
    }
    lock.lock();
    release_frame_data(frame.data);
}

void UsrQueueAsync::skip_gap_locked(uint32_t next_available_seq)
{
    if (!primed_ || next_available_seq <= expected_seq_)
        return;

    const uint32_t missing = next_available_seq - expected_seq_;
    VIDEO_DEBUG_PRINT(
        "[JitterBuf] gap skip expected=%u next=%u miss=%u depth=%zu avg_fi=%.2fms jitter=%.2fms disorder=%.2f",
        expected_seq_, next_available_seq, missing, buffered_frames_.size(), avg_interval_ms_, jitter_ms_,
        disorder_depth_frames_);

    counters_.skip += missing;

    expected_seq_ = next_available_seq;
    gap_active_ = false;
    next_delivery_time_ = std::chrono::steady_clock::now();
}

void UsrQueueAsync::drain_locked(std::unique_lock<std::mutex> &lock)
{
    while (!buffered_frames_.empty())
    {
        auto frame = buffered_frames_.front();
        buffered_frames_.pop_front();
        lock.unlock();

        try
        {
            if (receive_callback_)
                receive_callback_(frame.data, frame.size);
        }
        catch (const std::exception &e)
        {
            VIDEO_ERROR_PRINT("[JitterBuf] receive callback threw exception while draining: %s", e.what());
        }
        catch (...)
        {
            VIDEO_ERROR_PRINT("[JitterBuf] receive callback threw unknown exception while draining");
        }

        lock.lock();
        release_frame_data(frame.data);
    }
}

// ReliableUDP implementation
ReliableUDP::ReliableUDP(asio::io_context &io_context, unsigned short local_port)
    : running_(false), io_context_(io_context), strand_(io_context), receive_buffer_(new uint8_t[MAX_TRX_UDP_SIZE]),
      send_pool_(25), recv_pool_(25), recv_packets_(0), rs_(nullptr), target_uid_(0), destination_set_(false),
      frame_cycle_(1), group_cycle_(1), next_group_id_(1), next_frame_id_(1), last_frame_id_(0), last_group_id_(0),
      usr_queue_(std::make_unique<UsrQueueAsync>()), lost_packets_(0), send_bytes_(0), recv_bytes_(0),
      last_send_rate_time_(std::chrono::steady_clock::now()), last_recv_rate_time_(std::chrono::steady_clock::now()),
      last_lost_rate_time_(std::chrono::steady_clock::now()), last_send_rate_bytes_(0), last_recv_rate_bytes_(0),
      probe_timer_(io_context_), rtt_ms_(-1), offset_ms_(0), time_synced_(false), probe_seq_(0)
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

std::string ReliableUDP::queue_stats_text() const
{
    return usr_queue_->stats_text();
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

std::vector<TRXUnit> ReliableUDP::create_trx_units(const uint8_t *data, size_t size)
{
    constexpr auto max_group_data_size = MAX_RS_PACKET_NUM_PER_GROUP * MAX_TRX_DATA_SIZE;
    const size_t total_groups = (size + max_group_data_size - 1) / max_group_data_size;

    std::vector<TRXUnit> all_units;
    size_t offset = 0;
    uint16_t current_frame_id = next_frame_id_++;
    uint16_t current_group_id = (next_group_id_ >= 0xffffu - MAX_GROUP_NUM_PER_FRAME) ? 0 : next_group_id_;

    for (size_t group_idx = 0; group_idx < total_groups; ++group_idx)
    {
        size_t remaining_size = size - offset;
        size_t current_group_size = std::min(remaining_size, max_group_data_size);

        if (current_group_size > MAX_TRX_DATA_SIZE)
        {
            create_huge_rtx_group(all_units, data + offset, current_group_size, current_group_id, target_uid_,
                                  static_cast<uint16_t>(current_frame_id), static_cast<uint16_t>(total_groups));
        }
        else
        {
            create_small_rtx_group(all_units, data + offset, current_group_size, current_group_id, target_uid_,
                                   static_cast<uint16_t>(current_frame_id), static_cast<uint16_t>(total_groups));
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

    if (!unit.data)
    {
        return;
    }

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

    if (units_num != TRX_RS_FEC_GROUP_UNIT_NUMS || units_num != TRX_XOR_FEC_GROUP_UNIT_NUMS)
    {
        VIDEO_ERROR_PRINT("Unsupported units_num %u for group (%u,%u)", units_num, it->trxunit_cycle,
                          it->trxunit_group);
        recv_pool_.deallocate(unit.data);
    }

    if (!it->units.empty() && units_num != static_cast<uint8_t>(unit.head.units_num))
    {
        VIDEO_ERROR_PRINT("Inconsistent units_num for group (%u,%u)", it->trxunit_cycle, it->trxunit_group);
        recv_pool_.deallocate(unit.data);
        return;
    }

    it->set_received(idx);

    auto data_packets_count = 0;
    bool use_rs_fec = false;

    if (units_num == TRX_RS_FEC_GROUP_UNIT_NUMS)
    {
        data_packets_count = MAX_RS_PACKET_NUM_PER_GROUP;
        use_rs_fec = true;
    }
    else
    {
        data_packets_count = MAX_XOR_PACKET_NUM_PER_GROUP;
    }

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
        if (use_rs_fec)
        {
            auto units_copy = std::move(it->units);
            try_recover_group(units_copy, data_packets_count);
        }
        else
        {
            assemble_complete_message(unit.head.frame_seq, unit.head.group_num, unit.head.group_seq, unit.data,
                                      unit.head.group_len);
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
        auto oldest_layout_num =
            oldest_units_num == TRX_RS_FEC_GROUP_UNIT_NUMS ? TRX_RS_FEC_GROUP_UNIT_NUMS : TRX_XOR_FEC_GROUP_UNIT_NUMS;

        lost_packets_ +=
            static_cast<uint64_t>(oldest_layout_num - recv_cnt);

        for (auto &u : oldest_group.units)
        {
            recv_pool_.deallocate(u.data);
        }

        receive_groups_.pop_back();
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
        if (!complete_message)
        {
            VIDEO_ERROR_PRINT("Failed to allocate assembly buffer of %zu bytes", total_length);
            for (const auto &frag : it->fragments)
                recv_pool_.deallocate(frag.second);
            receive_frames_.erase(it);
            return;
        }
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
        }
        recv_pool_.deallocate(complete_message);
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
