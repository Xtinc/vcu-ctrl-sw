#ifndef RELIABLE_UDP_H
#define RELIABLE_UDP_H

#include "ClockWait.h"
#include "MemPoolUDP.h"
#include "ReedSoloman.h"
#include "asio.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

void set_current_thread_scheduler_policy();

class BackgroundService
{
  public:
    static BackgroundService &instance();
    asio::io_context &context();

  private:
    BackgroundService();
    ~BackgroundService();

    asio::io_context io_context;
    std::vector<std::thread> io_thds;
    asio::executor_work_guard<asio::io_context::executor_type> work_guard;
};

#define BG_SERVICE (BackgroundService::instance().context())

using RecvCallBack = std::function<void(const uint8_t *data, size_t size)>;

enum class TRXFECMode
{
    None,
    XOR,
    RS,
};

constexpr uint8_t MAGIC_TRX_PROBE_NUMBER = 0xA5;
constexpr size_t MAX_TRX_UNIT_SIZE = 1200;
constexpr size_t MAX_TRX_UDP_SIZE = 131072;
constexpr size_t MAX_RS_PACKET_NUM_PER_GROUP = 12;
constexpr size_t TRX_RS_FEC_REDUNDANCY = 3;
constexpr size_t MAX_XOR_PACKET_NUM_PER_GROUP = 1;
constexpr size_t TRX_XOR_FEC_REDUNDANCY = 1;
constexpr size_t MAX_TRX_RECEIVE_FRAMES = 20;
constexpr size_t MAX_GROUP_NUM_PER_FRAME = MAX_TRX_UDP_SIZE / (MAX_RS_PACKET_NUM_PER_GROUP * MAX_TRX_UNIT_SIZE);
static_assert(MAX_RS_PACKET_NUM_PER_GROUP + TRX_RS_FEC_REDUNDANCY <= 16, "units_num exceeds 4 bits");

struct TRXProbe
{
    uint8_t magic;       ///< Always MAGIC_TRX_PROBE_NUMBER (0xA5).
    uint8_t type;        ///< Packet type: 0 = ping, 1 = pong.
    uint8_t seq;         ///< Sequence number, wraps at 256.
    uint8_t pad;         ///< Reserved, set to 0.
    uint32_t t1_ms;      ///< ping: sender wall-clock time (ms mod 2^32); pong: echoed from ping.
    int32_t t2_delta_ms; ///< pong: (int32_t)(receiver_time - t1_ms), range ±2^31 ms; ping: 0.
};

/// @brief Transmission unit (UDP payload): a fixed-length Header followed by a variable-length data block.
struct TRXUnit
{
    /// @brief Fixed per-packet header, TRX_HEADER_SIZE bytes in total.
    struct Header
    {
        uint16_t frame_seq;      // 2 B - frame sequence number
        uint16_t group_seq;      // 2 B - group sequence number within a session
        uint16_t group_len;      // 2 B - total payload bytes in this group
        uint16_t group_num : 8;  // 8 b (0-255) - number of groups in this frame
        uint16_t units_idx : 4;  // 4 b (0-15)  - index of this unit within the group
        uint16_t units_num : 4;  // 4 b (0-15)  - total units in the group (data + FEC)
        uint32_t units_len : 12; // 12 b - payload length of this unit
        uint32_t conn_uuid : 12; // 12 b - connection UUID
        uint32_t check_sum : 8;  //  8 b - Adler-8 checksum over the preceding header bytes
    } head;
    uint8_t *data;

    static uint8_t adler_8(Header *header)
    {
        uint8_t a = 1;
        uint8_t b = 0;
        constexpr uint8_t MOD = 251;
        auto data = reinterpret_cast<uint8_t *>(header);
        constexpr auto length = sizeof(Header) - 1;

        for (size_t i = 0; i < length; i++)
        {
            a = (a + data[i]) % MOD;
            b = (b + a) % MOD;
        }

        return (b << 4) | (a & 0x0F);
    }

    static bool validate(Header *header)
    {
        if (header->units_num != 1 && header->units_num != MAX_XOR_PACKET_NUM_PER_GROUP + TRX_XOR_FEC_REDUNDANCY &&
            header->units_num != MAX_RS_PACKET_NUM_PER_GROUP + TRX_RS_FEC_REDUNDANCY)
        {
            return false;
        }
        if (header->units_idx >= header->units_num)
        {
            return false;
        }

        return header->check_sum == adler_8(header);
    }
};

/// @brief Receiver-side context that accumulates units belonging to the same group,
///        tracking a received-unit bitmap and a stale-group timeout timestamp.
struct TRXGroup
{
    uint16_t trxunit_cycle;
    uint16_t trxunit_group;
    uint16_t trxuint_guuid;
    uint16_t trxunit_recvd;
    std::chrono::steady_clock::time_point timestamp;
    std::vector<TRXUnit> units;
    bool loss_confirmed_and_counted = false;

    bool has_received(size_t idx) const
    {
        return (trxunit_recvd & (1ull << idx)) != 0;
    }

    void set_received(size_t idx)
    {
        trxunit_recvd |= (1ull << idx);
    }

    uint16_t received_cnt() const
    {
        static uint16_t table[256] = {
            0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4, 1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5,
            1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
            1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
            2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
            1, 2, 2, 3, 2, 3, 3, 4, 2, 3, 3, 4, 3, 4, 4, 5, 2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6,
            2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
            2, 3, 3, 4, 3, 4, 4, 5, 3, 4, 4, 5, 4, 5, 5, 6, 3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7,
            3, 4, 4, 5, 4, 5, 5, 6, 4, 5, 5, 6, 5, 6, 6, 7, 4, 5, 5, 6, 5, 6, 6, 7, 5, 6, 6, 7, 6, 7, 7, 8};
        return table[trxunit_recvd & 0xFF] + table[(trxunit_recvd >> 8) & 0xFF];
    }
};

/// @brief Receiver-side context that collects reassembled groups belonging to the same frame;
///        assembles the complete frame once all groups have arrived.
struct TRXFrame
{
    using Fragment = std::pair<uint64_t, uint8_t *>;
    uint16_t frame_cyc;
    uint16_t frame_seq;
    uint16_t group_num;
    uint16_t recvd_num;
    std::vector<Fragment> fragments;

    TRXFrame(uint16_t frame_cyc, uint16_t frame_seq, uint16_t group_num, uint32_t group_seq, uint8_t *data, size_t size)
        : frame_cyc(frame_cyc), frame_seq(frame_seq), group_num(group_num), recvd_num(1)
    {
        uint64_t packed_info = (static_cast<uint64_t>(group_seq) << 32) | static_cast<uint32_t>(size);
        fragments.reserve(group_num);
        fragments.emplace_back(packed_info, data);
    }

    static uint32_t extract_group_seq(uint64_t packed_info)
    {
        return static_cast<uint32_t>(packed_info >> 32);
    }

    static uint32_t extract_size(uint64_t packed_info)
    {
        return static_cast<uint32_t>(packed_info & 0xFFFFFFFF);
    }
};

constexpr size_t TRX_HEADER_SIZE = sizeof(TRXUnit::Header);
constexpr size_t MAX_TRX_DATA_SIZE = MAX_TRX_UNIT_SIZE - TRX_HEADER_SIZE;

class ReliableUDP;
template <bool ASYNC> class UsrQueue;

template <> class UsrQueue<true>
{
    struct cb_data
    {
        uint8_t *data;
        size_t size;
        uint32_t seq;
    };

    static constexpr size_t MAX_JITTER_DEPTH = 8;
    static constexpr size_t MAX_REORDER_BUF = MAX_JITTER_DEPTH * 4;
    static constexpr std::chrono::milliseconds FLUSH_TIMEOUT{200};

  public:
    UsrQueue() : owner_(nullptr), running_(false), next_deliver_seq_(0), target_depth_(0)
    {
    }

    ~UsrQueue()
    {
        stop();
    }

    void set_owner(ReliableUDP *owner)
    {
        owner_ = owner;
    }

    void start(RecvCallBack callback, void *user_data)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_)
            return;

        receive_callback_ = callback;
        user_data_ = user_data;
        running_ = true;
        thread_ = std::thread(&UsrQueue::worker_thread, this);
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        cond_.notify_all();
        if (thread_.joinable())
            thread_.join();
    }

    bool enqueue(uint8_t *data, size_t size, uint32_t abs_seq)
    {
        if (size > MAX_TRX_UDP_SIZE || data == nullptr)
            return false;

        std::lock_guard<std::mutex> lock(mutex_);

        if (reorder_buf_.size() >= MAX_REORDER_BUF)
            return false;

        auto result = reorder_buf_.emplace(abs_seq, cb_data{data, size, abs_seq});
        if (!result.second)
            return false; // duplicate seq

        // Disorder metric: 0-based position of this frame in the sorted buffer
        // at insertion time.  Position 0 = arrived in order; position k = k
        // earlier frames are already buffered.
        double pos = static_cast<double>(std::distance(reorder_buf_.begin(), result.first));
        disorder_hist_.add(pos);

        cond_.notify_one();
        return true;
    }

    /// Returns the current adaptive jitter buffer depth (in frames).
    size_t target_depth() const
    {
        return target_depth_.load(std::memory_order_relaxed);
    }

    /// Returns the raw P90 disorder quantile (in frames).
    /// Returns NaN when no samples have been collected yet (boot_n == 0).
    double disorder_p90() const
    {
        std::lock_guard<std::mutex> lk(mutex_);
        return disorder_hist_.quantile(0.90);
    }

  private:
    void worker_thread()
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
                flush_at = clock::now() + FLUSH_TIMEOUT;
        }
    }

    bool try_deliver_locked(std::unique_lock<std::mutex> &lock, bool force)
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
            // decrease: P90 < 0.5 maps to 0, 0.5–1.5 maps to 1, etc.
            // ceil would keep depth at 1 even when p90 ≈ 0.001 (mostly
            // in-order traffic), preventing the buffer from recovering.
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

  private:
    ReliableUDP *owner_;
    RecvCallBack receive_callback_;
    void *user_data_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    bool running_;

    std::map<uint32_t, cb_data> reorder_buf_;
    uint32_t next_deliver_seq_;
    std::atomic<size_t> target_depth_;
    Histogram<32> disorder_hist_;
};

template <> class UsrQueue<false>
{
  public:
    UsrQueue() : owner_(nullptr)
    {
    }

    void set_owner(ReliableUDP *owner)
    {
        owner_ = owner;
    }

    void start(RecvCallBack callback, void *user_data)
    {
        receive_callback_ = callback;
        user_data_ = user_data;
    }

    void stop()
    {
        receive_callback_ = nullptr;
        user_data_ = nullptr;
    }

    bool enqueue(uint8_t *data, size_t size, uint32_t /*abs_seq*/)
    {
        if (size > MAX_TRX_UDP_SIZE || data == nullptr)
            return false;

        receive_callback_(data, size);
        if (owner_)
            owner_->release_recv_buf(data);
        return true;
    }

  private:
    ReliableUDP *owner_;
    RecvCallBack receive_callback_;
    void *user_data_;
};

/**
 * @brief UDP-based reliable transport layer with integrated FEC error correction
 *        and NTP-style clock synchronisation.
 *
 * ### Framing and FEC rules
 * Each send() call corresponds to one frame.  The frame is split into one or more
 * **groups** of at most `MAX_RS_PACKET_NUM_PER_GROUP × MAX_TRX_DATA_SIZE` bytes;
 * each group independently selects its error-correction mode based on its size:
 *
 * | Group size                    | Mode | Data pkts | Redundancy pkts | Recoverable losses |
 * |-------------------------------|------|-----------|-----------------|--------------------|
 * | <= MAX_TRX_UNIT_SIZE (1200 B) | XOR  | 1         | 1 (full copy)   | 1                  |
 * | >  MAX_TRX_UNIT_SIZE          | RS   | 12        | 3               | up to 3            |
 *
 * RS mode uses Reed-Solomon(12, 3): any 12 received packets (data or parity)
 * are sufficient to reconstruct the full group.
 *
 * ### RTT and clock offset
 * Uses a simplified NTP four-timestamp model where T3 ≈ T2 (pong is sent
 * immediately upon receiving the ping):
 * @code
 *   RTT    = T4 - T1
 *   offset = (T2 - T1) - RTT / 2   // (T2 - T1) == TRXProbe::t2_delta_ms
 * @endcode
 * A TRXProbe ping is sent every 5 seconds; statistics are updated when the pong
 * is received.  A positive offset means the remote clock is ahead of the local
 * clock; a negative offset means the local clock is ahead.
 */
class ReliableUDP : public std::enable_shared_from_this<ReliableUDP>
{
    using udp_socket_ptr = std::unique_ptr<asio::ip::udp::socket>;

  public:
    explicit ReliableUDP(asio::io_context &io_context, unsigned short local_port);
    ~ReliableUDP();

    void start();
    void stop();
    bool add_destination(const std::string &address, unsigned short port);
    bool send(const uint8_t *data, size_t size);
    void set_receive_callback(RecvCallBack callback, void *user_data = nullptr)
    {
        usr_queue_.start(callback, user_data);
    }
    void release_recv_buf(uint8_t *p);
    double send_rate();
    double recv_rate();
    double lost_rate();

    int64_t rtt_ms() const;
    int64_t offset_ms() const;
    bool is_time_synced() const;

  private:
    void start_receive();
    void handle_receive(const asio::error_code &error, size_t bytes_transferred);
    void create_no_fec_group(std::vector<TRXUnit> &all_units, const uint8_t *data, size_t current_group_size,
                             uint16_t current_group_id, uint16_t uid, uint16_t current_frame_id, uint16_t total_groups);
    std::vector<TRXUnit> create_trx_units(const uint8_t *data, size_t size);
    void create_huge_rtx_group(std::vector<TRXUnit> &all_units, const uint8_t *data, size_t current_group_size,
                               uint16_t current_group_id, uint16_t uid, uint16_t current_frame_id,
                               uint16_t total_groups);
    void create_small_rtx_group(std::vector<TRXUnit> &all_units, const uint8_t *data, size_t current_group_size,
                                uint16_t current_group_id, uint16_t uid, uint16_t current_frame_id,
                                uint16_t total_groups);

    void process_received_unit(const TRXUnit &unit);
    void try_recover_group(std::vector<TRXUnit> &units, uint8_t data_packets_count);
    void assemble_complete_message(uint16_t frame_seq, uint16_t group_num, uint16_t group_seq, uint8_t *data,
                                   size_t size);
    void generate_uuid();
    TRXFECMode resolve_fec_mode(size_t packet_size) const;

    void schedule_probe();
    void send_probe();
    void handle_probe_packet(const TRXProbe &pkt);
    static uint32_t get_time_ms();

  private:
    std::atomic<bool> running_;
    asio::io_context &io_context_;
    asio::io_context::strand strand_;
    udp_socket_ptr recv_socket_;
    udp_socket_ptr send_socket_;
    asio::ip::udp::endpoint remote_endpoint_;
    uint8_t *receive_buffer_;
    MemPool<6, 16> send_pool_;
    MemPool<6, 16> recv_pool_;
    std::atomic<uint64_t> recv_packets_;

    reed_solomon *rs_;

    asio::ip::udp::endpoint target_endpoint_;
    uint16_t target_uid_;
    bool destination_set_;
    std::mutex target_mutex_;

    uint16_t frame_cycle_;
    uint16_t group_cycle_;
    uint16_t next_group_id_;
    uint16_t next_frame_id_;
    uint16_t last_frame_id_;
    uint16_t last_group_id_;
    std::list<TRXGroup> receive_groups_;
    std::list<TRXFrame> receive_frames_;

    UsrQueue<true> usr_queue_;
    std::atomic<uint64_t> lost_packets_;
    std::atomic<uint64_t> send_bytes_;
    std::atomic<uint64_t> recv_bytes_;
    std::mutex rate_mutex_;
    std::chrono::steady_clock::time_point last_send_rate_time_;
    std::chrono::steady_clock::time_point last_recv_rate_time_;
    uint64_t last_send_rate_bytes_;
    uint64_t last_recv_rate_bytes_;

    asio::steady_timer probe_timer_;
    std::atomic<int64_t> rtt_ms_;
    std::atomic<int64_t> offset_ms_;
    std::atomic<bool> time_synced_;
    uint8_t probe_seq_;
};

#endif // RELIABLE_UDP_H