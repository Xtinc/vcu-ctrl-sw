#ifndef RELIABLE_UDP_H
#define RELIABLE_UDP_H

#include "QueueAsync.h"
#include "CSVWriter.h"
#include "ReedSoloman.h"
#include "asio.hpp"

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

constexpr uint8_t MAGIC_TRX_PROBE_NUMBER = 0xA5;
constexpr size_t MAX_TRX_UNIT_SIZE = 1200;
constexpr size_t MAX_TRX_UDP_SIZE = 65535;
constexpr size_t MAX_RS_PACKET_NUM_PER_GROUP = 12;
constexpr size_t TRX_RS_FEC_REDUNDANCY = 3;
constexpr size_t MAX_XOR_PACKET_NUM_PER_GROUP = 2;
constexpr size_t TRX_XOR_FEC_REDUNDANCY = 1;
constexpr size_t MAX_TRX_RECEIVE_FRAMES = 20;
constexpr size_t MAX_GROUP_NUM_PER_FRAME = MAX_TRX_UDP_SIZE / (MAX_RS_PACKET_NUM_PER_GROUP * MAX_TRX_UNIT_SIZE);
static_assert(MAX_RS_PACKET_NUM_PER_GROUP + TRX_RS_FEC_REDUNDANCY <= 16, "units_num exceeds 4 bits");
static_assert(MAX_XOR_PACKET_NUM_PER_GROUP + TRX_XOR_FEC_REDUNDANCY <= 16, "units_num exceeds 4 bits");
static_assert(SEND_QUEUE_MAX_PACKET_SIZE == MAX_TRX_UDP_SIZE,
              "send queue packet size must match ReliableUDP frame cap");

struct TRXProbe
{
    uint8_t magic;       ///< Always MAGIC_TRX_PROBE_NUMBER (0xA5).
    uint8_t type;        ///< Packet type: 0 = ping, 1 = pong.
    uint8_t seq;         ///< Sequence number, wraps at 256.
    uint8_t pad;         ///< Reserved, set to 0.
    uint32_t t1_ms;      ///< ping: sender wall-clock time (ms mod 2^32); pong: echoed from ping.
    int32_t t2_delta_ms; ///< pong: (int32_t)(receiver_time - t1_ms), range ±2^31 ms; ping: 0.
};

static_assert(std::is_standard_layout<TRXProbe>::value, "TRXProbe must remain a standard-layout wire structure");
static_assert(sizeof(TRXProbe) == 12, "TRXProbe wire layout changed");
static_assert(alignof(TRXProbe) == 4, "TRXProbe alignment changed");
static_assert(offsetof(TRXProbe, t1_ms) == 4, "TRXProbe::t1_ms offset changed");
static_assert(offsetof(TRXProbe, t2_delta_ms) == 8, "TRXProbe::t2_delta_ms offset changed");

struct ClockSync
{
    ClockSync();

    int64_t rtt_ms() const;
    int64_t offset_ms() const;
    bool is_time_synced() const;

    TRXProbe make_ping(uint32_t now_ms);
    TRXProbe make_pong(const TRXProbe &ping, uint32_t now_ms) const;
    void mark_ping_sent(const TRXProbe &ping, std::chrono::steady_clock::time_point sent_time);
    bool handle_pong(const TRXProbe &pong);

    std::atomic<int64_t> rtt_ms_;
    std::atomic<int64_t> offset_ms_;
    std::atomic<bool> time_synced_;
    uint8_t probe_seq_;
    bool has_pending_probe_;
    uint8_t pending_probe_seq_;
    uint32_t pending_probe_t1_ms_;
    std::chrono::steady_clock::time_point pending_probe_sent_time_;
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

static_assert(std::is_standard_layout<TRXUnit::Header>::value,
              "TRXUnit::Header must remain a standard-layout wire structure");
static_assert(sizeof(TRXUnit::Header) == 12, "TRXUnit::Header wire layout changed");
static_assert(alignof(TRXUnit::Header) == 4, "TRXUnit::Header alignment changed");
static_assert(offsetof(TRXUnit::Header, frame_seq) == 0, "TRXUnit::Header::frame_seq offset changed");
static_assert(offsetof(TRXUnit::Header, group_seq) == 2, "TRXUnit::Header::group_seq offset changed");
static_assert(offsetof(TRXUnit::Header, group_len) == 4, "TRXUnit::Header::group_len offset changed");

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
 * | <= MAX_TRX_DATA_SIZE          | XOR  | 2         | 1               | 1                  |
 * | >  MAX_TRX_DATA_SIZE          | RS   | 12        | 3               | up to 3            |
 *
 * XOR mode splits a small payload into two padded halves and sends one parity
 * packet (`half0 ^ half1`), so any two of the three packets reconstruct the
 * original payload.
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
    bool send_fill(size_t size, FillCallback callback);
    void set_receive_callback(RecvCallBack callback);
    double send_rate();
    double recv_rate();
    double lost_rate();

    int64_t rtt_ms() const;
    int64_t offset_ms() const;
    bool is_time_synced() const;

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
    void reset_receive_state_for_new_epoch(uint16_t uid);
    void try_recover_group(std::vector<TRXUnit> &units, uint8_t data_packets_count);
    void try_recover_xor_group(std::vector<TRXUnit> &units);
    void try_recover_rs_group(std::vector<TRXUnit> &units, uint8_t data_packets_count);
    void assemble_complete_message(uint16_t frame_seq, uint16_t group_num, uint16_t group_seq, uint8_t *data,
                                   size_t size);
    void generate_uuid();
    void send_ping_probe();
    void schedule_probe();
    bool send_probe_packet(const TRXProbe &pkt, const char *label);
    void send_queued_frame(const uint8_t *data, size_t size);
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
    bool has_active_conn_uuid_;
    uint16_t active_conn_uuid_;
    bool destination_set_;
    std::mutex target_mutex_;

    uint16_t frame_cycle_;
    uint16_t group_cycle_;
    uint16_t next_group_id_;
    uint16_t next_frame_id_;
    uint16_t last_frame_id_;
    uint16_t last_group_id_;
    bool has_last_receive_activity_;
    std::chrono::steady_clock::time_point last_receive_activity_;
    std::list<TRXGroup> receive_groups_;
    std::list<TRXFrame> receive_frames_;

    std::unique_ptr<RecvQueueAsync> usr_queue_;
    std::unique_ptr<SendQueueAsync> send_queue_;
    std::atomic<uint64_t> lost_packets_;
    std::atomic<uint64_t> send_bytes_;
    std::atomic<uint64_t> recv_bytes_;
    std::mutex rate_mutex_;
    std::mutex send_socket_mutex_;
    std::chrono::steady_clock::time_point last_send_rate_time_;
    std::chrono::steady_clock::time_point last_recv_rate_time_;
    std::chrono::steady_clock::time_point last_lost_rate_time_;
    uint64_t last_send_rate_bytes_;
    uint64_t last_recv_rate_bytes_;

    asio::steady_timer probe_timer_;
    ClockSync clock_sync_;
    NetCSVWriter stats_writer_;
};

#endif // RELIABLE_UDP_H
