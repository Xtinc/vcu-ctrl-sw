#ifndef RELIABLE_UDP_H
#define RELIABLE_UDP_H

#include "asio.hpp"
#include "udp_mem.h"
#include "udp_rs.h"

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

using RecvCallBack = std::function<void(const uint8_t *data, size_t size)>;

enum class TRXFecMode
{
    None,
    XOR,
    RS,
};

constexpr size_t MAX_TRX_UNIT_SIZE = 1200;
constexpr size_t MAX_TRX_UDP_SIZE = 65536;
constexpr size_t MAX_RS_PACKET_NUM_PER_GROUP = 12;
constexpr size_t TRX_RS_FEC_REDUNDANCY = 3;
constexpr size_t MAX_XOR_PACKET_NUM_PER_GROUP = 1;
constexpr size_t TRX_XOR_FEC_REDUNDANCY = 1;
constexpr size_t MAX_TRX_RECEIVE_FRAMES = 20;
constexpr size_t MAX_GROUP_NUM_PER_FRAME = MAX_TRX_UDP_SIZE / (MAX_RS_PACKET_NUM_PER_GROUP * MAX_TRX_UNIT_SIZE);
static_assert(MAX_RS_PACKET_NUM_PER_GROUP + TRX_RS_FEC_REDUNDANCY <= 16, "units_num exceeds 4 bits");

using asio::ip::udp;

struct TRXUnit
{
    struct Header
    {
        uint16_t frame_seq; // 2字节 - 帧序列号
        uint16_t group_seq; // 2字节 - 组序列号
        uint16_t group_len; // 2字节 - 组总长度

        uint16_t group_num : 8; // 8位 (0-255) - 帧中组数量
        uint16_t units_idx : 4; // 4位 (0-15) - 组内单元索引
        uint16_t units_num : 4; // 4位 (0-15) - 组内单元总数

        uint32_t units_len : 12; // 12位 - 单元实际长度
        uint32_t conn_uuid : 12; // 12位 - 连接UUID
        uint32_t check_sum : 8;  // 8位 - 校验和
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

template <bool ASYNC> class UsrQueue;

template <> class UsrQueue<true>
{
    using cb_data = std::pair<uint8_t *, size_t>;

  public:
    UsrQueue() : running_(false), write_idx_(0), read_idx_(0)
    {
        for (auto &p : queue_)
        {
            p.first = new uint8_t[MAX_TRX_UDP_SIZE];
            p.second = 0;
        }
    }

    ~UsrQueue()
    {
        stop();
        for (auto &p : queue_)
        {
            delete[] p.first;
        }
    }

    void start(RecvCallBack callback, void *user_data)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_)
        {
            return;
        }

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
        {
            thread_.join();
        }
    }

    bool enqueue(const uint8_t *data, size_t size)
    {
        if (size > MAX_TRX_UDP_SIZE || data == nullptr)
        {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        size_t next_write = (write_idx_ + 1) % queue_.size();
        if (next_write == read_idx_)
        {
            return false;
        }

        std::memcpy(queue_[write_idx_].first, data, size);
        queue_[write_idx_].second = size;
        write_idx_ = next_write;

        cond_.notify_one();
        return true;
    }

  private:
    void worker_thread()
    {
        while (true)
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this] { return !running_ || read_idx_ != write_idx_; });

            if (!running_ && read_idx_ == write_idx_)
            {
                break;
            }

            if (read_idx_ != write_idx_)
            {
                cb_data data = queue_[read_idx_];
                read_idx_ = (read_idx_ + 1) % queue_.size();
                lock.unlock();
                receive_callback_(data.first, data.second);
            }
        }
    }

  private:
    RecvCallBack receive_callback_;
    void *user_data_;

    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cond_;
    bool running_;
    size_t write_idx_;
    size_t read_idx_;
    std::array<cb_data, 30> queue_;
};

template <> class UsrQueue<false>
{
  public:
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

    bool enqueue(const uint8_t *data, size_t size)
    {
        if (size > MAX_TRX_UDP_SIZE || data == nullptr)
        {
            return false;
        }

        receive_callback_(data, size);
        return true;
    }

  private:
    RecvCallBack receive_callback_;
    void *user_data_;
};

class ReliableUDP : public std::enable_shared_from_this<ReliableUDP>
{
    using udp_socket_ptr = std::unique_ptr<udp::socket>;

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
    double send_rate();
    double recv_rate();
    double lost_rate();

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
    TRXFecMode resolve_fec_mode(size_t packet_size) const;

  private:
    std::atomic<bool> running_;
    asio::io_context &io_context_;
    asio::io_context::strand strand_;
    udp_socket_ptr recv_socket_;
    udp_socket_ptr send_socket_;
    udp::endpoint remote_endpoint_;
    uint8_t *receive_buffer_;
    MemPool<6, 16> send_pool_;
    MemPool<6, 16> recv_pool_;
    std::atomic<uint64_t> recv_packets_;

    reed_solomon *rs_;

    udp::endpoint target_endpoint_;
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

    UsrQueue<false> usr_queue_;
    std::atomic<uint64_t> lost_packets_;
    std::atomic<uint64_t> send_bytes_;
    std::atomic<uint64_t> recv_bytes_;
    std::mutex rate_mutex_;
    std::chrono::steady_clock::time_point last_send_rate_time_;
    std::chrono::steady_clock::time_point last_recv_rate_time_;
    uint64_t last_send_rate_bytes_;
    uint64_t last_recv_rate_bytes_;
};

#endif // RELIABLE_UDP_H