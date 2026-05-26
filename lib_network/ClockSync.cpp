#include "ClockSync.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <cstring>

ClockSync::ClockSync() : socket_(io_ctx_), timer_(io_ctx_)
{
}

ClockSync::~ClockSync()
{
    io_ctx_.stop();
    if (io_thread_.joinable())
    {
        io_thread_.join();
    }
}

void ClockSync::start_server(uint16_t port)
{
    try
    {
        is_server_ = true;
        socket_.open(asio::ip::udp::v4());
        socket_.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), port));

        VIDEO_INFO_PRINT("[ClockSync] Server listening on port %u", port);

        async_receive_request();
        io_thread_ = std::thread(&ClockSync::io_thread_func, this);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("[ClockSync] Failed to start server: %s", e.what());
    }
}

void ClockSync::start_client(const std::string &server_ip, uint16_t port)
{
    try
    {
        is_server_ = false;
        socket_.open(asio::ip::udp::v4());
        server_endpoint_ = asio::ip::udp::endpoint(asio::ip::address::from_string(server_ip), port);

        VIDEO_INFO_PRINT("[ClockSync] Client connecting to %s:%u", server_ip.c_str(), port);
        async_send_probe();
        schedule_calibration();
        io_thread_ = std::thread(&ClockSync::io_thread_func, this);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("[ClockSync] Failed to start client: %s", e.what());
    }
}

void ClockSync::io_thread_func()
{
    try
    {
        io_ctx_.run();
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("[ClockSync] IO thread error: %s", e.what());
    }
}

void ClockSync::schedule_calibration()
{
    timer_.expires_after(std::chrono::seconds(30));
    timer_.async_wait([this](const asio::error_code &ec) {
        if (!ec)
        {
            async_send_probe();
            schedule_calibration();
        }
    });
}

void ClockSync::async_receive_request()
{
    socket_.async_receive_from(asio::buffer(recv_buffer_), remote_endpoint_,
                               [this](const asio::error_code &ec, std::size_t bytes_received) {
                                   if (!ec && bytes_received >= sizeof(SyncMessage))
                                   {
                                       SyncMessage msg;
                                       std::memcpy(&msg, recv_buffer_.data(), sizeof(SyncMessage));

                                       if (is_server_ && msg.type == 0)
                                       {
                                           // Server: handle sync request
                                           handle_request(msg, remote_endpoint_);
                                       }
                                       else if (!is_server_ && msg.type == 1)
                                       {
                                           // Client: handle sync response
                                           uint64_t t4 = get_time_us();
                                           handle_response(msg, t4);
                                       }
                                   }
                                   async_receive_request();
                               });
}

void ClockSync::async_send_probe()
{
    SyncMessage msg;
    msg.type = 0;
    std::memset(msg.reserved, 0, sizeof(msg.reserved));
    msg.t1 = get_time_us();
    msg.t2 = 0;
    msg.t3 = 0;

    auto send_buffer = std::make_shared<std::array<uint8_t, sizeof(SyncMessage)>>();
    std::memcpy(send_buffer->data(), &msg, sizeof(SyncMessage));

    socket_.async_send_to(asio::buffer(*send_buffer), server_endpoint_,
                          [send_buffer](const asio::error_code &ec, std::size_t /*bytes_sent*/) {
                              if (ec)
                              {
                                  VIDEO_ERROR_PRINT("[ClockSync] Failed to send probe: %s", ec.message().c_str());
                              }
                          });
}

void ClockSync::handle_request(const SyncMessage &req, const asio::ip::udp::endpoint &client_endpoint)
{
    uint64_t t2 = get_time_us();

    SyncMessage resp;
    resp.type = 1;
    std::memset(resp.reserved, 0, sizeof(resp.reserved));
    resp.t1 = req.t1;
    resp.t2 = t2;
    resp.t3 = get_time_us();

    auto send_buffer = std::make_shared<std::array<uint8_t, sizeof(SyncMessage)>>();
    std::memcpy(send_buffer->data(), &resp, sizeof(SyncMessage));

    socket_.async_send_to(asio::buffer(*send_buffer), client_endpoint,
                          [send_buffer](const asio::error_code &ec, std::size_t /*bytes_sent*/) {
                              if (ec)
                              {
                                  VIDEO_ERROR_PRINT("[ClockSync] Failed to send response: %s", ec.message().c_str());
                              }
                          });
}

void ClockSync::handle_response(const SyncMessage &resp, uint64_t t4)
{
    // offset = ((T2 - T1) + (T3 - T4)) / 2
    // rtt = (T4 - T1) - (T3 - T2)

    int64_t t1 = static_cast<int64_t>(resp.t1);
    int64_t t2 = static_cast<int64_t>(resp.t2);
    int64_t t3 = static_cast<int64_t>(resp.t3);
    int64_t t4_signed = static_cast<int64_t>(t4);

    int64_t offset = ((t2 - t1) + (t3 - t4_signed)) / 2;
    int64_t rtt = (t4_signed - t1) - (t3 - t2);

    offset_us_.store(offset, std::memory_order_relaxed);
    rtt_us_.store(rtt, std::memory_order_relaxed);
    synchronized_.store(true, std::memory_order_relaxed);

    VIDEO_ERROR_PRINT("[ClockSync] Offset: %.3f ms, RTT: %.3f ms", offset / 1e3, rtt / 1e3);
}

uint64_t ClockSync::get_time_us()
{
    auto now = std::chrono::steady_clock::now();
    auto duration = now.time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(duration).count());
}
