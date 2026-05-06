#ifndef CLOCK_SYNC_H
#define CLOCK_SYNC_H

#include <asio.hpp>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>

/**
 * @brief Network time synchronization using simplified NTP algorithm
 *
 * Provides clock offset estimation between encoder and decoder devices
 * over UDP. Uses asio for async I/O and periodic calibration.
 *
 * Protocol:
 * - Encoder runs as server, waiting for sync requests
 * - Decoder runs as client, sending periodic probe messages
 * - Calculates clock offset using NTP algorithm:
 *   offset = ((T2-T1) - (T4-T3)) / 2
 *   rtt = (T4-T1) - (T3-T2)
 *
 * Thread safety:
 * - get_offset_ns() and get_rtt_ns() are lock-free atomic reads
 * - asio operations run in dedicated thread
 */
class ClockSync
{
  public:
    ClockSync();
    ~ClockSync();

    // Disable copy and move
    ClockSync(const ClockSync &) = delete;
    ClockSync &operator=(const ClockSync &) = delete;

    /**
     * @brief Start as server (encoder side)
     * @param port UDP port to listen on
     */
    void start_server(uint16_t port);

    /**
     * @brief Start as client (decoder side)
     * @param server_ip Encoder IP address
     * @param port Encoder UDP port
     */
    void start_client(const std::string &server_ip, uint16_t port);

    /**
     * @brief Get current clock offset in nanoseconds
     * @return offset (decoder_time - encoder_time)
     */
    int64_t get_offset_ns() const
    {
        return offset_ns_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Get last measured round-trip time in nanoseconds
     * @return RTT in nanoseconds
     */
    int64_t get_rtt_ns() const
    {
        return rtt_ns_.load(std::memory_order_relaxed);
    }

    /**
     * @brief Check if synchronization is active
     */
    bool is_synchronized() const
    {
        return synchronized_.load(std::memory_order_relaxed);
    }

  private:
    struct alignas(8) SyncMessage
    {
        uint8_t type; // 0=request, 1=response
        uint8_t reserved[7];
        uint64_t t1; // Client send time (ns)
        uint64_t t2; // Server receive time (ns)
        uint64_t t3; // Server send time (ns)
    };

    static_assert(alignof(SyncMessage) == 8, "SyncMessage alignment must be 8 bytes");
    static_assert(sizeof(SyncMessage) == 32, "SyncMessage size must be 32 bytes");
    static_assert(offsetof(SyncMessage, t1) == 8, "SyncMessage::t1 offset must be 8");
    static_assert(offsetof(SyncMessage, t2) == 16, "SyncMessage::t2 offset must be 16");
    static_assert(offsetof(SyncMessage, t3) == 24, "SyncMessage::t3 offset must be 24");

    void io_thread_func();
    void schedule_calibration();
    void async_receive_request();
    void async_send_probe();
    void handle_request(const SyncMessage &req, const asio::ip::udp::endpoint &client_endpoint);
    void handle_response(const SyncMessage &resp, uint64_t t4);

    static uint64_t get_time_ns();

    asio::io_context io_ctx_;
    asio::ip::udp::socket socket_;
    asio::steady_timer timer_;
    asio::ip::udp::endpoint server_endpoint_;
    asio::ip::udp::endpoint remote_endpoint_;

    std::atomic<int64_t> offset_ns_{0};
    std::atomic<int64_t> rtt_ns_{0};
    std::atomic<bool> synchronized_{false};

    std::thread io_thread_;
    bool is_server_{false};

    std::array<uint8_t, sizeof(SyncMessage)> recv_buffer_;
};

#endif // CLOCK_SYNC_H
