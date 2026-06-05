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

constexpr uint8_t MAGIC_TRX_PROBE_NUMBER = 0xA5;
constexpr size_t MAX_TRX_UNIT_SIZE = 1200;
constexpr size_t MAX_TRX_UDP_SIZE = 65535;
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

struct TRXUnit
{
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

/** @brief Asynchronous fixed-depth jitter buffer for fully reassembled frames.
 *
 * This queue trades latency for continuity. It keeps a steady frame cushion
 * near its configured target depth, reorders buffered frames by absolute
 * sequence number, and feeds the decoder at the long-term average receive cadence.
 *
 * Behaviour model:
 *  - Startup prefill waits until the configured startup depth is buffered.
 *  - Normal delivery is paced by a long-term receive-interval estimator.
 *  - Occupancy feedback nudges the delivery interval so buffered depth stays
 *    near the configured target depth instead of draining to zero or growing
 *    unbounded.
 *  - When a sequence gap blocks delivery, the queue waits for late arrival up
 *    to a jitter/reorder-aware deadline, then skips the missing span.
 *
 * Queue statistics are exposed through stats_text(); reading them never mutates
 * queue state. Counters reset only when the queue state is reset.
 */
class UsrQueueAsync
{
    using ClockTP = ClockEntry::ClockTP;
    struct Tunables
    {
        size_t target_depth = 10;
        size_t startup_depth = 10;
        size_t max_buffered_frames = 64;
        double stale_timeout_ms = 2000.0;
        double default_frame_interval_ms = 16.0;
        double depth_feedback_gain = 0.08;
        double min_pacing_factor = 0.70;
        double max_pacing_factor = 1.30;
    };

    struct BufferedFrame
    {
        uint32_t seq;
        uint8_t *data;
        size_t size;
        ClockTP arrival;
    };

    struct Counters
    {
        uint64_t recv = 0;
        uint64_t deliver = 0;
        uint64_t skip = 0;
        uint64_t drop = 0;
        uint64_t duplicate = 0;
        uint64_t late = 0;
        uint64_t reorder = 0;
        uint64_t stale = 0;
        uint64_t overflow = 0;
        uint32_t max_disorder_depth = 0;
    };

  public:
    UsrQueueAsync();
    ~UsrQueueAsync();

    void start(RecvCallBack callback);
    void stop();
    bool enqueue(uint8_t *data, size_t size, uint32_t abs_seq);
    std::string stats_text() const;

  private:
    void worker_thread();
    void sanitize_tuning_locked();
    void reset_state_locked();
    void drain_locked(std::unique_lock<std::mutex> &lock);
    void update_estimators_locked(uint32_t abs_seq, ClockTP arrival);
    void update_depth_estimate_locked(double depth);
    void note_disorder_locked(uint32_t abs_seq);
    void purge_stale_locked(ClockTP now);
    void drop_frame_locked(std::list<BufferedFrame>::iterator it);
    void release_frame_data(uint8_t *data);
    double bootstrap_interval_locked() const;
    double compute_delivery_interval_locked() const;
    double compute_gap_wait_ms_locked() const;
    void deliver_one_locked(std::unique_lock<std::mutex> &lock);
    void skip_gap_locked(uint32_t next_available_seq);

    MemPool<6, 16> frame_pool_;
    RecvCallBack receive_callback_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    bool running_;

    std::list<BufferedFrame> buffered_frames_;
    bool primed_;
    uint32_t expected_seq_;
    ClockTP next_delivery_time_;
    bool gap_active_;
    ClockTP gap_start_time_;
    Tunables tuning_;

    bool has_arrival_ref_;
    uint32_t last_rate_seq_;
    ClockTP last_rate_time_;
    bool has_highest_arrival_seq_;
    uint32_t highest_arrival_seq_;
    double avg_interval_ms_;
    double jitter_ms_;
    double avg_depth_frames_;
    double disorder_depth_frames_;
    double disorder_guard_depth_frames_;
    Counters counters_;
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
    void set_receive_callback(RecvCallBack callback)
    {
        usr_queue_->start(callback);
    }
    double send_rate();
    double recv_rate();
    double lost_rate();

    int64_t rtt_ms() const;
    int64_t offset_ms() const;
    bool is_time_synced() const;
    std::string queue_stats_text() const;

  private:
    void start_receive();
    void handle_receive(const asio::error_code &error, size_t bytes_transferred);

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
    void schedule_probe();
    void send_probe();
    void handle_probe_packet(const TRXProbe &pkt);

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

    std::unique_ptr<UsrQueueAsync> usr_queue_;
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