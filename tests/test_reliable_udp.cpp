/**
 * @file test_reliable_udp.cpp
 * @brief Loopback send/receive unit test for ReliableUDP.
 *
 * Topology
 * --------
 *   Sender (port 15001)
 *       │  UDP datagrams
 *       ▼
 *   LossyProxy (port 15010)
 *       │  drops each datagram with probability ∈ [loss_min, loss_max]
 *       ▼
 *   Receiver (port 15002)
 *
 * Command-line options (all optional)
 * ------------------------------------
 *   --min-bytes <n>     minimum payload size in bytes (default: 64)
 *   --max-bytes <n>     maximum payload size in bytes (default: 10000)
 *   --rate-mbps <f>     target transmit throughput in Mbps  (default: 5.0)
 *   --loss-min  <f>     minimum per-datagram drop probability [0..1] (default: 0.0)
 *   --loss-max  <f>     maximum per-datagram drop probability [0..1] (default: 0.05)
 *   --duration  <n>     test duration in seconds  (default: 10)
 *
 * Exit code: 0 = all received messages passed integrity check,  1 = failures.
 */

#include "lib_network/ReliableUDP.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// ─── Test configuration ───────────────────────────────────────────────────────

struct TestConfig
{
    size_t   min_payload_bytes = 64;
    size_t   max_payload_bytes = 10000;
    double   target_rate_bps   = 5e6;  ///< bits/s; 0 = unlimited
    double   loss_rate_min     = 0.0;  ///< fraction [0,1]
    double   loss_rate_max     = 0.05; ///< fraction [0,1]
    uint32_t duration_sec      = 10;
    uint16_t sender_port       = 15001;
    uint16_t proxy_port        = 15010;
    uint16_t receiver_port     = 15002;
};

// ─── Integrity-tagged message header ──────────────────────────────────────────

#pragma pack(push, 1)
struct TestMsgHdr
{
    uint32_t seq;         ///< monotonically increasing sequence number
    uint32_t payload_crc; ///< Adler-32 over the payload bytes following this header
    uint32_t payload_len; ///< number of payload bytes
};
#pragma pack(pop)

static constexpr size_t TEST_HDR_SIZE = sizeof(TestMsgHdr);

static uint32_t adler32_compute(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i)
    {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

// ─── Lossy UDP proxy ──────────────────────────────────────────────────────────

/**
 * Listens on @p listen_port.  Each received UDP datagram is forwarded to
 * @p dst_port on 127.0.0.1 after surviving a random drop whose probability is
 * drawn uniformly from [loss_rate_min, loss_rate_max].
 */
class LossyProxy
{
  public:
    LossyProxy(asio::io_context &ioc, uint16_t listen_port, uint16_t dst_port, double loss_rate_min,
               double loss_rate_max)
        : socket_(ioc, asio::ip::udp::endpoint(asio::ip::udp::v4(), listen_port)),
          dst_(asio::ip::make_address("127.0.0.1"), dst_port), rng_(std::random_device{}()),
          rate_dist_(loss_rate_min, loss_rate_max), drop_dist_(0.0, 1.0),
          buf_(new uint8_t[MAX_TRX_UDP_SIZE]), dropped_(0), forwarded_(0)
    {
        do_receive();
    }

    ~LossyProxy()
    {
        asio::error_code ec;
        socket_.close(ec);
        delete[] buf_;
    }

    LossyProxy(const LossyProxy &)            = delete;
    LossyProxy &operator=(const LossyProxy &) = delete;

    uint64_t dropped() const
    {
        return dropped_.load(std::memory_order_relaxed);
    }
    uint64_t forwarded() const
    {
        return forwarded_.load(std::memory_order_relaxed);
    }

  private:
    void do_receive()
    {
        socket_.async_receive_from(
            asio::buffer(buf_, MAX_TRX_UDP_SIZE), sender_ep_,
            [this](const asio::error_code &ec, size_t n) {
                if (ec == asio::error::operation_aborted)
                {
                    return;
                }
                if (!ec && n > 0)
                {
                    double loss_prob;
                    double draw;
                    {
                        std::lock_guard<std::mutex> lk(rng_mtx_);
                        loss_prob = rate_dist_(rng_);
                        draw      = drop_dist_(rng_);
                    }

                    if (draw < loss_prob)
                    {
                        dropped_.fetch_add(1, std::memory_order_relaxed);
                    }
                    else
                    {
                        forwarded_.fetch_add(1, std::memory_order_relaxed);
                        asio::error_code send_ec;
                        socket_.send_to(asio::buffer(buf_, n), dst_, 0, send_ec);
                    }
                }
                do_receive();
            });
    }

    asio::ip::udp::socket      socket_;
    asio::ip::udp::endpoint    sender_ep_;
    asio::ip::udp::endpoint    dst_;
    std::mt19937               rng_;
    std::mutex                 rng_mtx_;
    std::uniform_real_distribution<double> rate_dist_;
    std::uniform_real_distribution<double> drop_dist_;
    uint8_t                   *buf_;
    std::atomic<uint64_t>      dropped_;
    std::atomic<uint64_t>      forwarded_;
};

// ─── Receiver-side state ──────────────────────────────────────────────────────

struct TestState
{
    std::mutex              recv_mtx;
    std::condition_variable recv_cv;
    uint64_t                send_count{0};     ///< written only by sender thread before wait
    std::atomic<uint64_t>   recv_count{0};
    std::atomic<uint64_t>   recv_errors{0};    ///< integrity failures
    std::atomic<uint64_t>   recv_dup{0};       ///< duplicate sequence numbers
    std::vector<bool>       seq_seen;          ///< indexed by seq; resized in run_test

    void on_receive(const uint8_t *data, size_t size)
    {
        if (size < TEST_HDR_SIZE)
        {
            recv_errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        TestMsgHdr hdr{};
        std::memcpy(&hdr, data, TEST_HDR_SIZE);
        const uint8_t *payload = data + TEST_HDR_SIZE;
        const size_t   plen    = size - TEST_HDR_SIZE;

        // Length consistency
        if (plen != hdr.payload_len)
        {
            recv_errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Integrity check
        if (adler32_compute(payload, plen) != hdr.payload_crc)
        {
            recv_errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Duplicate detection
        {
            std::lock_guard<std::mutex> lk(recv_mtx);
            if (hdr.seq < seq_seen.size())
            {
                if (seq_seen[hdr.seq])
                {
                    recv_dup.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                seq_seen[hdr.seq] = true;
            }
        }

        recv_count.fetch_add(1, std::memory_order_relaxed);
        recv_cv.notify_one();
    }
};

// ─── Pretty output helpers ────────────────────────────────────────────────────

static void print_divider()
{
    std::cout << std::string(62, '-') << '\n';
}

static void print_results(const TestConfig &cfg, const TestState &state, uint64_t dropped, uint64_t forwarded,
                          double elapsed_sec)
{
    auto sent   = state.send_count;
    auto recvd  = state.recv_count.load();
    auto errors = state.recv_errors.load();
    auto dups   = state.recv_dup.load();

    double msg_loss_pct =
        sent > 0 ? 100.0 * (1.0 - static_cast<double>(recvd) / static_cast<double>(sent)) : 0.0;
    uint64_t total_udp  = dropped + forwarded;
    double   udp_loss_pct = total_udp > 0 ? 100.0 * static_cast<double>(dropped) / static_cast<double>(total_udp)
                                           : 0.0;

    std::cout << '\n';
    print_divider();
    std::cout << "  ReliableUDP Test Results\n";
    print_divider();
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Duration          : " << elapsed_sec << " s\n";
    std::cout << "  Payload range     : " << cfg.min_payload_bytes << " – " << cfg.max_payload_bytes << " bytes\n";
    std::cout << "  Target rate       : " << cfg.target_rate_bps / 1e6 << " Mbps\n";
    std::cout << "  Loss range        : " << cfg.loss_rate_min * 100.0 << "% – " << cfg.loss_rate_max * 100.0
              << "%\n";
    print_divider();
    std::cout << "  Messages sent     : " << sent << "\n";
    std::cout << "  Messages received : " << recvd << "  (loss: " << msg_loss_pct << "%)\n";
    std::cout << "  Duplicate msgs    : " << dups << "\n";
    std::cout << "  Integrity errors  : " << errors << "\n";
    std::cout << "  UDP pkts forwarded: " << forwarded << "\n";
    std::cout << "  UDP pkts dropped  : " << dropped << "  (" << udp_loss_pct << "%)\n";
    print_divider();
    // FEC recovery analysis
    // If FEC did nothing:  message_loss ≈ udp_loss
    // FEC recovery rate = udp_loss - message_loss  (both in %)
    // i.e. what fraction of the originally-lost UDP traffic the codec rescued.
    double fec_recovered_pct = udp_loss_pct - msg_loss_pct;
    if (fec_recovered_pct < 0.0) fec_recovered_pct = 0.0;
    double fec_efficiency_pct =
        udp_loss_pct > 0.0 ? 100.0 * fec_recovered_pct / udp_loss_pct : 100.0;
    std::cout << "  UDP pkt loss      : " << udp_loss_pct << "%\n";
    std::cout << "  Message loss      : " << msg_loss_pct << "%\n";
    std::cout << "  FEC recovered     : " << fec_recovered_pct
              << "% of traffic  (efficiency: " << fec_efficiency_pct << "%)\n";
    print_divider();

    const bool pass = (errors == 0 && dups == 0);
    if (pass)
    {
        std::cout << "  PASS: all received messages verified OK\n";
    }
    else
    {
        std::cout << "  FAIL:";
        if (errors > 0)
        {
            std::cout << "  " << errors << " integrity error(s)";
        }
        if (dups > 0)
        {
            std::cout << "  " << dups << " duplicate(s)";
        }
        std::cout << '\n';
    }
    print_divider();
    std::cout << '\n';
}

// ─── Core test runner ─────────────────────────────────────────────────────────

static void run_test(const TestConfig &cfg)
{
    auto &ioc = BG_SERVICE;

    // ── Receiver ────────────────────────────────────────────────────────────
    auto receiver = std::make_shared<ReliableUDP>(ioc, cfg.receiver_port);

    TestState state;
    // Pre-size the seen-bitmap generously (16-bit seq wraps at 65536)
    state.seq_seen.assign(65536, false);

    receiver->set_receive_callback([&state](const uint8_t *data, size_t size) { state.on_receive(data, size); });
    receiver->start();

    // ── Lossy proxy ─────────────────────────────────────────────────────────
    LossyProxy proxy(ioc, cfg.proxy_port, cfg.receiver_port, cfg.loss_rate_min, cfg.loss_rate_max);

    // ── Sender ──────────────────────────────────────────────────────────────
    auto sender = std::make_shared<ReliableUDP>(ioc, cfg.sender_port);
    sender->start();

    if (!sender->add_destination("127.0.0.1", cfg.proxy_port))
    {
        throw std::runtime_error("add_destination failed");
    }

    // ── Send loop ────────────────────────────────────────────────────────────
    std::mt19937                              rng(0xDEADBEEFu);
    std::uniform_int_distribution<size_t>     size_dist(cfg.min_payload_bytes, cfg.max_payload_bytes);

    const double bytes_per_sec = cfg.target_rate_bps > 0.0 ? cfg.target_rate_bps / 8.0 : 0.0;
    const auto   test_start    = std::chrono::steady_clock::now();
    const auto   deadline      = test_start + std::chrono::seconds(cfg.duration_sec);

    double bytes_budget       = bytes_per_sec > 0.0 ? bytes_per_sec : 0.0; // one second of initial credit
    auto   last_budget_update = test_start;

    std::vector<uint8_t> msg_buf;
    msg_buf.reserve(cfg.max_payload_bytes + TEST_HDR_SIZE);

    uint32_t seq = 0;

    while (std::chrono::steady_clock::now() < deadline)
    {
        const size_t payload_size = size_dist(rng);
        const size_t total_size   = payload_size + TEST_HDR_SIZE;

        // ── Rate limiting ─────────────────────────────────────────────────
        if (bytes_per_sec > 0.0)
        {
            const auto   now = std::chrono::steady_clock::now();
            const double dt  = std::chrono::duration<double>(now - last_budget_update).count();
            bytes_budget += dt * bytes_per_sec;
            last_budget_update = now;

            if (bytes_budget < static_cast<double>(total_size))
            {
                const double wait_sec = (static_cast<double>(total_size) - bytes_budget) / bytes_per_sec;
                std::this_thread::sleep_for(std::chrono::duration<double>(wait_sec));
                continue;
            }
            bytes_budget -= static_cast<double>(total_size);
        }

        // ── Build test message ────────────────────────────────────────────
        msg_buf.resize(total_size);
        uint8_t *payload_ptr = msg_buf.data() + TEST_HDR_SIZE;

        // Deterministic payload: byte[i] = (seq ^ i) & 0xFF
        for (size_t i = 0; i < payload_size; ++i)
        {
            payload_ptr[i] = static_cast<uint8_t>((seq ^ static_cast<uint32_t>(i)) & 0xFFu);
        }

        TestMsgHdr hdr{};
        hdr.seq         = seq;
        hdr.payload_len = static_cast<uint32_t>(payload_size);
        hdr.payload_crc = adler32_compute(payload_ptr, payload_size);
        std::memcpy(msg_buf.data(), &hdr, TEST_HDR_SIZE);

        if (sender->send(msg_buf.data(), total_size))
        {
            state.send_count++;
            seq++;
        }
    }

    // ── Flush window: wait up to 5 s for in-flight messages to arrive ─────
    {
        const auto flush_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::unique_lock<std::mutex> lk(state.recv_mtx);
        state.recv_cv.wait_until(lk, flush_deadline,
                                 [&state] { return state.recv_count.load() >= state.send_count; });
    }

    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - test_start).count();

    sender->stop();
    receiver->stop();

    print_results(cfg, state, proxy.dropped(), proxy.forwarded(), elapsed);

    if (state.recv_errors.load() > 0 || state.recv_dup.load() > 0)
    {
        throw std::runtime_error("Data integrity or duplicate check failed");
    }
}

// ─── Entry point ─────────────────────────────────────────────────────────────

static void usage(const char *prog)
{
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --min-bytes <n>     minimum payload bytes (default: 64)\n"
              << "  --max-bytes <n>     maximum payload bytes (default: 10000)\n"
              << "  --rate-mbps <f>     transmit rate in Mbps  (default: 5.0)\n"
              << "  --loss-min  <f>     min drop probability   (default: 0.0)\n"
              << "  --loss-max  <f>     max drop probability   (default: 0.05)\n"
              << "  --duration  <n>     test duration seconds  (default: 10)\n";
}

int main(int argc, char *argv[])
{
    TestConfig cfg;

    for (int i = 1; i < argc; ++i)
    {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h")
        {
            usage(argv[0]);
            return 0;
        }

        if (i + 1 >= argc)
        {
            std::cerr << "Missing value for " << arg << '\n';
            usage(argv[0]);
            return 1;
        }

        try
        {
            if (arg == "--min-bytes")
            {
                cfg.min_payload_bytes = static_cast<size_t>(std::stoul(argv[++i]));
            }
            else if (arg == "--max-bytes")
            {
                cfg.max_payload_bytes = static_cast<size_t>(std::stoul(argv[++i]));
            }
            else if (arg == "--rate-mbps")
            {
                cfg.target_rate_bps = std::stod(argv[++i]) * 1e6;
            }
            else if (arg == "--loss-min")
            {
                cfg.loss_rate_min = std::stod(argv[++i]);
            }
            else if (arg == "--loss-max")
            {
                cfg.loss_rate_max = std::stod(argv[++i]);
            }
            else if (arg == "--duration")
            {
                cfg.duration_sec = static_cast<uint32_t>(std::stoul(argv[++i]));
            }
            else
            {
                std::cerr << "Unknown option: " << arg << '\n';
                usage(argv[0]);
                return 1;
            }
        }
        catch (const std::exception &e)
        {
            std::cerr << "Bad value for " << arg << ": " << e.what() << '\n';
            return 1;
        }
    }

    // ── Clamp / validate ─────────────────────────────────────────────────────
    if (cfg.min_payload_bytes < TEST_HDR_SIZE)
    {
        cfg.min_payload_bytes = TEST_HDR_SIZE;
    }
    const size_t max_allowed = MAX_TRX_UDP_SIZE - TEST_HDR_SIZE;
    if (cfg.max_payload_bytes > max_allowed)
    {
        cfg.max_payload_bytes = max_allowed;
    }
    if (cfg.min_payload_bytes > cfg.max_payload_bytes)
    {
        cfg.max_payload_bytes = cfg.min_payload_bytes;
    }
    if (cfg.loss_rate_min < 0.0)
    {
        cfg.loss_rate_min = 0.0;
    }
    if (cfg.loss_rate_max > 1.0)
    {
        cfg.loss_rate_max = 1.0;
    }
    if (cfg.loss_rate_min > cfg.loss_rate_max)
    {
        cfg.loss_rate_max = cfg.loss_rate_min;
    }

    std::cout << "ReliableUDP loopback test\n";
    std::cout << "  Payload  : " << cfg.min_payload_bytes << " – " << cfg.max_payload_bytes << " bytes\n";
    std::cout << "  Rate     : " << std::fixed << std::setprecision(2) << cfg.target_rate_bps / 1e6 << " Mbps\n";
    std::cout << "  Loss     : " << cfg.loss_rate_min * 100.0 << "% – " << cfg.loss_rate_max * 100.0 << "%\n";
    std::cout << "  Duration : " << cfg.duration_sec << " s\n\n";

    try
    {
        run_test(cfg);
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
