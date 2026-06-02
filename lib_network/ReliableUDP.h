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

/// @brief Abstract base for user delivery queues.
///
/// Concrete implementations differ in how they dispatch received frames to the
/// application callback:
/// - UsrQueueAsync  — background thread with adaptive jitter/reorder buffer
/// - UsrQueueSync   — direct call inside the receive path (no buffering)
class UsrQueue
{
  public:
    virtual ~UsrQueue() = default;

    virtual void set_owner(ReliableUDP *owner) = 0;
    virtual void start(RecvCallBack callback, void *user_data) = 0;
    virtual void stop() = 0;
    virtual bool enqueue(uint8_t *data, size_t size, uint32_t abs_seq) = 0;

    /// Returns the current adaptive jitter buffer depth (frames).
    /// Default implementation for queues without buffering returns 0.
    virtual size_t target_depth() const
    {
        return 0;
    }

    /// Returns the P90 disorder quantile (frames).
    /// Default implementation returns NaN (no statistics available).
    virtual double disorder_p90() const
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
};

/// @brief Asynchronous delivery queue with adaptive jitter/reorder buffer.
///
/// Design (GStreamer rtpjitterbuffer inspired), tuned for 60 fps video:
///   - Every enqueued frame records its steady-clock arrival time.
///   - The worker sleeps via cond_.wait_until(deadline) — no busy-wait.
///     Deadline is computed per-iteration:
///       in-order front  → last_deliver_time + frame_interval_ms  (pacing)
///       gap front       → arrival_time + playout_latency_ms      (gap wait)
///   - A frame is delivered when any of the following is true:
///       in_order     : it carries the next expected sequence number
///       depth_reached: buffer size > target_depth_ (adaptive P90 of reorder
///                      distances); signals the missing predecessor is lost
///       timed_out    : arrival_time + playout_latency_ms elapsed (last resort)
///   - Playout latency is adapted via RFC 3550 §6.4.1 EWMA on inter-arrival
///     timing, clamped to [MIN_PLAYOUT_MS, MAX_PLAYOUT_MS].
///   - Delivery is paced at the EWMA inter-frame interval so buffered frames
///     are handed to the decoder at a smooth, steady rate.
///   - On shutdown, all remaining buffered frames are drained synchronously.
class UsrQueueAsync : public UsrQueue
{
    using ClockTP = std::chrono::steady_clock::time_point;

    struct cb_data
    {
        uint8_t *data;
        size_t size;
        uint32_t seq;
        ClockTP arrival_time;
    };

    static const size_t MAX_JITTER_DEPTH; ///< hard cap on target_depth_ (8 × 16.67ms = 133ms @ 60fps)
    static const size_t MAX_REORDER_BUF;
    static const int64_t MIN_PLAYOUT_MS;       ///< minimum gap-flush timeout: ~5 frames @ 500fps (10ms)
    static const int64_t MAX_PLAYOUT_MS;       ///< cap on adaptive playout delay
    static const double MIN_FRAME_INTERVAL_MS; ///< pacing floor (~1000 fps max)

  public:
    UsrQueueAsync();
    ~UsrQueueAsync() override;

    void set_owner(ReliableUDP *owner) override;
    void start(RecvCallBack callback, void *user_data) override;
    void stop() override;
    bool enqueue(uint8_t *data, size_t size, uint32_t abs_seq) override;

    /// Returns the current adaptive reorder buffer target depth (= ceil P90 disorder distance).
    size_t target_depth() const override;
    /// Returns target_depth() as a double for monitoring interfaces.
    double disorder_p90() const override;

  private:
    void worker_thread();
    void try_deliver_locked(std::unique_lock<std::mutex> &lock);
    void update_jitter_locked(uint32_t abs_seq, ClockTP arrival);

    ReliableUDP *owner_;
    RecvCallBack receive_callback_;
    void *user_data_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    bool running_;

    std::map<uint32_t, cb_data> reorder_buf_;
    uint32_t next_deliver_seq_;

    // RFC 3550 inter-arrival jitter estimator (protected by mutex_)
    bool has_timing_ref_;
    uint32_t last_timing_seq_;
    ClockTP last_arrival_time_;
    double jitter_ms_;

    std::atomic<int64_t> playout_latency_ms_;

    // Disorder histogram: samples reorder distance (abs_seq - next_deliver_seq_)
    // for each arriving frame.  ceil(P90) drives target_depth_ so depth_reached
    // fires well before the playout timeout when the link has persistent reordering.
    Histogram<32> disorder_hist_;      // protected by mutex_
    std::atomic<size_t> target_depth_; // ceil(P90 disorder), load/store relaxed

    // Delivery pacing: EWMA of inter-delivery interval, used to compute the
    // worker deadline when the front frame is already in-order (no gap waiting
    // needed) so buffered frames are played out at a smooth, steady rate.
    ClockTP last_deliver_time_; // protected by mutex_
    bool has_deliver_ref_;      // whether last_deliver_time_ is valid
    double frame_interval_ms_;  // EWMA inter-delivery interval, protected by mutex_
};

/// @brief Synchronous delivery queue — no buffering, no background thread.
///
/// enqueue() calls the application callback directly on the caller's thread
/// and releases the buffer immediately after the callback returns.
class UsrQueueSync : public UsrQueue
{
  public:
    UsrQueueSync();
    ~UsrQueueSync() override = default;

    void set_owner(ReliableUDP *owner) override;
    void start(RecvCallBack callback, void *user_data) override;
    void stop() override;
    bool enqueue(uint8_t *data, size_t size, uint32_t abs_seq) override;

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
 * Each send() call corresponds to one frame. If the payload fits in one TRX data
 * packet (`MAX_TRX_DATA_SIZE` bytes), the frame uses XOR redundancy; otherwise it
 * uses Reed-Solomon for every group in the frame. Large payloads are split into one
 * or more groups of at most `MAX_RS_PACKET_NUM_PER_GROUP × MAX_TRX_DATA_SIZE` bytes.
 *
 * | Frame payload size            | Mode | Data pkts | Redundancy pkts | Recoverable losses |
 * |-------------------------------|------|-----------|-----------------|--------------------|
 * | <= MAX_TRX_DATA_SIZE          | XOR  | 1         | 1 (full copy)   | 1                  |
 * | >  MAX_TRX_DATA_SIZE          | RS   | 12        | 3               | up to 3            |
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
        usr_queue_->start(callback, user_data);
    }
    void release_recv_buf(uint8_t *p);
    double send_rate();
    double recv_rate();
    double lost_rate();
    void count_dropped_packet();

    int64_t rtt_ms() const;
    int64_t offset_ms() const;
    bool is_time_synced() const;
    size_t jitter_depth() const;

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

    std::unique_ptr<UsrQueue> usr_queue_;
    std::atomic<uint64_t> lost_packets_;
    std::atomic<uint64_t> send_bytes_;
    std::atomic<uint64_t> recv_bytes_;
    std::mutex rate_mutex_;
    std::chrono::steady_clock::time_point last_send_rate_time_;
    std::chrono::steady_clock::time_point last_recv_rate_time_;
    std::chrono::steady_clock::time_point last_lost_rate_time_;
    uint64_t last_send_rate_bytes_;
    uint64_t last_recv_rate_bytes_;

    asio::steady_timer probe_timer_;
    std::atomic<int64_t> rtt_ms_;
    std::atomic<int64_t> offset_ms_;
    std::atomic<bool> time_synced_;
    uint8_t probe_seq_;
};

#endif // RELIABLE_UDP_H