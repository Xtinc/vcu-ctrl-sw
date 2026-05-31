/**
 * @file test_reliable_udp.cpp
 * @brief Unit and integration tests for ReliableUDP.
 *
 * Two test suites are executed in sequence:
 *
 *   1. Jitter-buffer unit tests (UsrQueueAsync, no network)
 *      Verify in-order delivery, reorder recovery, duplicate suppression,
 *      disorder statistics, adaptive depth tracking, and gap-skip behaviour.
 *
 *   2. FEC loopback integration test (full ReliableUDP stack)
 *      A sender transmits variable-size messages through a lossy UDP proxy
 *      to a receiver.  Every received message is integrity-checked with
 *      Adler-32.  The test reports FEC recovery efficiency.
 *
 * Topology (integration test)
 * ---------------------------
 *   Sender (port 15001)
 *       |  UDP datagrams
 *       v
 *   LossyProxy (port 15010)  -- drops each datagram with probability in [loss_min, loss_max]
 *       |
 *       v
 *   Receiver (port 15002)
 *
 * Command-line options (all optional)
 * ------------------------------------
 *   --min-bytes <n>   minimum payload size in bytes   (default: 64)
 *   --max-bytes <n>   maximum payload size in bytes   (default: 10000)
 *   --rate-mbps <f>   target transmit rate in Mbps    (default: 5.0)
 *   --loss-min  <f>   minimum drop probability [0..1] (default: 0.0)
 *   --loss-max  <f>   maximum drop probability [0..1] (default: 0.05)
 *   --duration  <n>   test duration in seconds        (default: 10)
 *
 * Exit code: 0 = all tests passed, 1 = one or more failures.
 */

#include "lib_network/ReliableUDP.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void print_divider()
{
    std::cout << std::string(62, '-') << '\n';
}

// ---------------------------------------------------------------------------
// Jitter-buffer unit tests (UsrQueueAsync)
// ---------------------------------------------------------------------------

/// Enqueue N in-order frames; verify all are delivered in ascending order.
static bool test_jitter_inorder()
{
    constexpr uint32_t N = 50;
    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<uint32_t> received;
    std::mutex mtx;
    std::condition_variable cv;

    UsrQueueAsync q;
    q.start(
        [&](const uint8_t *data, size_t) {
            uint32_t v;
            std::memcpy(&v, data, 4);
            std::lock_guard<std::mutex> lk(mtx);
            received.push_back(v);
            cv.notify_one();
        },
        nullptr);

    for (uint32_t i = 0; i < N; i++)
        q.enqueue(bufs[i].data(), 4, i);

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(2), [&] { return received.size() >= N; });
    }
    q.stop();

    if (received.size() != N)
        return false;
    for (size_t i = 1; i < received.size(); i++)
        if (received[i] < received[i - 1])
            return false;
    return true;
}

/// Deliver 50 frames with pairwise-swap reorder (1,0,3,2,...) and verify
/// every sequence number is received exactly once.
static bool test_jitter_reorder_complete()
{
    constexpr uint32_t N = 50;
    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<uint32_t> received;
    std::mutex mtx;
    std::condition_variable cv;

    UsrQueueAsync q;
    q.start(
        [&](const uint8_t *data, size_t) {
            uint32_t v;
            std::memcpy(&v, data, 4);
            std::lock_guard<std::mutex> lk(mtx);
            received.push_back(v);
            cv.notify_one();
        },
        nullptr);

    for (uint32_t i = 0; i < N; i += 2)
    {
        q.enqueue(bufs[i + 1].data(), 4, i + 1);
        q.enqueue(bufs[i].data(), 4, i);
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(2), [&] { return received.size() >= N; });
    }
    q.stop();

    if (received.size() != N)
    {
        std::cout << "  [reorder] expected " << N << " frames, got " << received.size() << '\n';
        return false;
    }
    std::set<uint32_t> recv_set(received.begin(), received.end());
    for (uint32_t i = 0; i < N; i++)
    {
        if (!recv_set.count(i))
        {
            std::cout << "  [reorder] seq " << i << " missing\n";
            return false;
        }
    }
    return true;
}

/// No duplicate deliveries: enqueue each frame twice; only one delivery per seq.
static bool test_jitter_no_duplicates()
{
    constexpr uint32_t N = 20;
    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<uint32_t> received;
    std::mutex mtx;
    std::condition_variable cv;

    UsrQueueAsync q;
    q.start(
        [&](const uint8_t *data, size_t) {
            uint32_t v;
            std::memcpy(&v, data, 4);
            std::lock_guard<std::mutex> lk(mtx);
            received.push_back(v);
            cv.notify_one();
        },
        nullptr);

    for (uint32_t i = 0; i < N; i++)
    {
        q.enqueue(bufs[i].data(), 4, i);
        q.enqueue(bufs[i].data(), 4, i); // duplicate — must be silently dropped
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(2), [&] { return received.size() >= N; });
    }
    q.stop();

    if (received.size() != N)
    {
        std::cout << "  [no_dup] expected " << N << " frames, got " << received.size() << '\n';
        return false;
    }
    std::set<uint32_t> recv_set(received.begin(), received.end());
    if (recv_set.size() != N)
    {
        std::cout << "  [no_dup] duplicates detected: " << received.size() - recv_set.size() << " extra deliveries\n";
        return false;
    }
    return true;
}

/// disorder_p90() returns NaN before any frame is enqueued.
static bool test_jitter_p90_empty()
{
    UsrQueueAsync q;
    return std::isnan(q.disorder_p90());
}

/// disorder_p90() returns a valid non-negative number after >= 32 samples.
/// Frames are enqueued without starting the worker so they accumulate
/// simultaneously, giving the histogram a non-trivial dataset.
static bool test_jitter_p90_valid()
{
    UsrQueueAsync q;
    static uint8_t dummy[4] = {};
    // 64 in-order frames all buffered at once → disorder positions 0,1,...,63
    for (uint32_t i = 0; i < 64; i++)
        q.enqueue(dummy, 4, i);

    double p90 = q.disorder_p90();
    return !std::isnan(p90) && p90 >= 0.0;
}

/// After sustained in-order traffic (well past bootstrap) target_depth must
/// converge to 0, confirming the depth can decrease.
static bool test_jitter_depth_recovers()
{
    constexpr uint32_t N = 200;
    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::mutex mtx;
    std::condition_variable cv;
    size_t delivered = 0;

    UsrQueueAsync q;
    q.start(
        [&](const uint8_t *, size_t) {
            std::lock_guard<std::mutex> lk(mtx);
            ++delivered;
            cv.notify_one();
        },
        nullptr);

    // Phase 1: 100 pairwise-swapped frames to push disorder up.
    constexpr uint32_t PHASE1 = 100;
    for (uint32_t i = 0; i < PHASE1; i += 2)
    {
        q.enqueue(bufs[i + 1].data(), 4, i + 1);
        q.enqueue(bufs[i].data(), 4, i);
    }
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(2), [&] { return delivered >= PHASE1; });
    }

    // Phase 2: 100 strictly in-order frames, paced 1 ms apart so the worker
    // advances next_deliver_seq_ before each successive frame arrives,
    // guaranteeing disorder == 0 for every phase-2 sample.
    for (uint32_t i = PHASE1; i < N; i++)
    {
        q.enqueue(bufs[i].data(), 4, i);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return delivered >= N; });
    }
    q.stop();

    if (delivered < N)
    {
        std::cout << "  [depth_recovers] expected " << N << " frames, got " << delivered << '\n';
        return false;
    }
    // After 100 paced in-order frames all disorder samples are 0;
    // EMA must converge: target_depth must be 0.
    if (q.target_depth() != 0)
    {
        std::cout << "  [depth_recovers] target_depth should be 0 after in-order phase, got " << q.target_depth()
                  << '\n';
        return false;
    }
    return true;
}

/// Simulate real network jitter via sliding-window shuffle (WINDOW = 6 frames).
/// Frames within each window arrive in a randomised order, as happens when a
/// router has per-packet variable queuing latency.
///
/// Checks:
///   1. All N frames are eventually delivered (depth_reached path rescues late arrivals).
///   2. The delivered sequence is non-decreasing (buffer restores order).
///   3. disorder_p90() is positive — confirms the histogram is tracking jitter.
static bool test_jitter_window_shuffle()
{
    constexpr uint32_t N = 300;
    constexpr uint32_t WINDOW = 6;

    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<uint32_t> received;
    std::mutex mtx;
    std::condition_variable cv;

    UsrQueueAsync q;
    q.start(
        [&](const uint8_t *data, size_t) {
            uint32_t v;
            std::memcpy(&v, data, 4);
            std::lock_guard<std::mutex> lk(mtx);
            received.push_back(v);
            cv.notify_one();
        },
        nullptr);

    std::mt19937 rng(0xCAFEBEEF);
    for (uint32_t base = 0; base < N; base += WINDOW)
    {
        uint32_t end = std::min(base + WINDOW, N);
        std::vector<uint32_t> win;
        for (uint32_t j = base; j < end; j++)
            win.push_back(j);
        std::shuffle(win.begin(), win.end(), rng);
        for (uint32_t seq : win)
            q.enqueue(bufs[seq].data(), 4, seq);
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(3), [&] { return received.size() >= N; });
    }
    q.stop();

    if (received.size() != N)
    {
        std::cout << "  [window_shuffle] expected " << N << " frames, got " << received.size() << '\n';
        return false;
    }
    for (size_t i = 1; i < received.size(); i++)
    {
        if (received[i] < received[i - 1])
        {
            std::cout << "  [window_shuffle] order violation at index " << i << ": seq " << received[i] << " after "
                      << received[i - 1] << '\n';
            return false;
        }
    }

    // Jitter-tracking sanity: after window-shuffle traffic the histogram must
    // have observed non-zero disorder.
    double p90 = q.disorder_p90();
    if (std::isnan(p90) || p90 <= 0.0)
    {
        std::cout << "  [window_shuffle] disorder_p90() = " << p90 << ", expected > 0\n";
        return false;
    }
    return true;
}

// Adaptive depth must rise during bursty jitter and fall back to zero once
// traffic returns to in-order.
//
/// Phase 1 (200 frames, window-5 shuffle): jitter is injected continuously.
///   → After all phase-1 frames are delivered, target_depth must be ≥ 1,
///     confirming the histogram drove the depth upward.
/// Phase 2 (200 frames, strict order): disorder drops to zero.
///   → After phase-2 drains, target_depth must be 0 (EMA has decayed).
static bool test_jitter_adapt_up_down()
{
    constexpr uint32_t PHASE1 = 200;
    constexpr uint32_t PHASE2 = 200;
    constexpr uint32_t TOTAL = PHASE1 + PHASE2;
    constexpr uint32_t WINDOW = 5;

    std::vector<std::array<uint8_t, 4>> bufs(TOTAL);
    for (uint32_t i = 0; i < TOTAL; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::mutex mtx;
    std::condition_variable cv;
    size_t delivered = 0;

    UsrQueueAsync q;
    q.start(
        [&](const uint8_t *, size_t) {
            std::lock_guard<std::mutex> lk(mtx);
            ++delivered;
            cv.notify_one();
        },
        nullptr);

    // Phase 1: window-shuffle disorder.
    std::mt19937 rng(0xDEADC0DE);
    for (uint32_t base = 0; base < PHASE1; base += WINDOW)
    {
        uint32_t end = std::min(base + WINDOW, PHASE1);
        std::vector<uint32_t> win;
        for (uint32_t j = base; j < end; j++)
            win.push_back(j);
        std::shuffle(win.begin(), win.end(), rng);
        for (uint32_t seq : win)
            q.enqueue(bufs[seq].data(), 4, seq);
    }
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(3), [&] { return delivered >= PHASE1; });
    }
    if (delivered < PHASE1)
        return false;

    // Sample depth while disorder is sustained.
    size_t depth_after_phase1 = q.target_depth();

    // Phase 2: strictly in-order frames, paced 1 ms apart so next_deliver_seq_
    // is current before each arrival, guaranteeing disorder == 0 for every sample.
    for (uint32_t i = PHASE1; i < TOTAL; i++)
    {
        q.enqueue(bufs[i].data(), 4, i);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return delivered >= TOTAL; });
    }
    q.stop();

    if (delivered < TOTAL)
    {
        std::cout << "  [adapt_up_down] expected " << TOTAL << " frames, got " << delivered << '\n';
        return false;
    }
    if (depth_after_phase1 < 1)
    {
        std::cout << "  [adapt_up_down] target_depth after shuffle phase = " << depth_after_phase1
                  << ", expected >= 1\n";
        return false;
    }
    if (q.target_depth() != 0)
    {
        std::cout << "  [adapt_up_down] target_depth after in-order phase = " << q.target_depth() << ", expected 0\n";
        return false;
    }
    return true;
}

/// A permanently missing frame (SKIP) must not stall delivery of all subsequent
/// frames.  The jitter buffer must skip the gap (via depth_reached or the
/// 200 ms flush timeout) and deliver every other frame exactly once.
static bool test_jitter_permanent_gap()
{
    constexpr uint32_t N = 20;
    constexpr uint32_t SKIP = 7; // this seq is never enqueued

    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<uint32_t> received;
    std::mutex mtx;
    std::condition_variable cv;

    UsrQueueAsync q;
    q.start(
        [&](const uint8_t *data, size_t) {
            uint32_t v;
            std::memcpy(&v, data, 4);
            std::lock_guard<std::mutex> lk(mtx);
            received.push_back(v);
            cv.notify_one();
        },
        nullptr);

    for (uint32_t i = 0; i < N; i++)
        if (i != SKIP)
            q.enqueue(bufs[i].data(), 4, i);

    // Wait up to 2× FLUSH_TIMEOUT (400 ms) for the gap to be skipped.
    const size_t expected = N - 1;
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(600), [&] { return received.size() >= expected; });
    }
    q.stop();

    if (received.size() != expected)
    {
        std::cout << "  [permanent_gap] expected " << expected << " frames, got " << received.size() << '\n';
        return false;
    }

    std::set<uint32_t> recv_set(received.begin(), received.end());
    if (recv_set.count(SKIP))
    {
        std::cout << "  [permanent_gap] skipped seq " << SKIP << " was incorrectly delivered\n";
        return false;
    }
    for (uint32_t i = 0; i < N; i++)
    {
        if (i != SKIP && !recv_set.count(i))
        {
            std::cout << "  [permanent_gap] seq " << i << " missing from output\n";
            return false;
        }
    }
    return true;
}

static int run_jitter_tests()
{
    struct Test
    {
        const char *name;
        bool (*fn)();
    };

    const Test tests[] = {
        {"in-order delivery preserves order   ", test_jitter_inorder},
        {"reorder: all frames received        ", test_jitter_reorder_complete},
        {"duplicate seq silently dropped      ", test_jitter_no_duplicates},
        {"disorder_p90() = NaN before samples ", test_jitter_p90_empty},
        {"disorder_p90() valid after bootstrap", test_jitter_p90_valid},
        {"target_depth recovers after reorder ", test_jitter_depth_recovers},
        {"window-shuffle: order+p90 tracking  ", test_jitter_window_shuffle},
        {"adaptive depth rises then falls     ", test_jitter_adapt_up_down},
        {"permanent gap skipped, rest deliver ", test_jitter_permanent_gap},
    };

    std::cout << "\nJitter buffer unit tests\n";
    print_divider();

    int failures = 0;
    for (const auto &t : tests)
    {
        bool ok = t.fn();
        std::cout << "  " << (ok ? "PASS" : "FAIL") << "  " << t.name << '\n';
        if (!ok)
            ++failures;
    }

    print_divider();
    if (failures == 0)
        std::cout << "  All unit tests passed.\n";
    else
        std::cout << "  " << failures << " unit test(s) FAILED.\n";
    print_divider();
    std::cout << '\n';

    return failures == 0 ? 0 : 1;
}

struct TestConfig
{
    size_t min_payload_bytes = 64;
    size_t max_payload_bytes = 10000;
    double target_rate_bps = 5e6; ///< bits/s; 0 = unlimited
    double loss_rate_min = 0.0;   ///< fraction [0,1]
    double loss_rate_max = 0.05;  ///< fraction [0,1]
    uint32_t duration_sec = 10;
    uint16_t sender_port = 15001;
    uint16_t proxy_port = 15010;
    uint16_t receiver_port = 15002;
};

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

/// Forwards each received datagram to dst_port with probability (1 - loss).
/// The loss probability is sampled uniformly from [loss_rate_min, loss_rate_max].
class LossyProxy
{
  public:
    LossyProxy(asio::io_context &ioc, uint16_t listen_port, uint16_t dst_port, double loss_rate_min,
               double loss_rate_max)
        : socket_(ioc, asio::ip::udp::endpoint(asio::ip::udp::v4(), listen_port)),
          dst_(asio::ip::make_address("127.0.0.1"), dst_port), rng_(std::random_device{}()),
          rate_dist_(loss_rate_min, loss_rate_max), drop_dist_(0.0, 1.0), buf_(new uint8_t[MAX_TRX_UDP_SIZE]),
          dropped_(0), forwarded_(0)
    {
        do_receive();
    }

    ~LossyProxy()
    {
        asio::error_code ec;
        socket_.close(ec);
        delete[] buf_;
    }

    LossyProxy(const LossyProxy &) = delete;
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
        socket_.async_receive_from(asio::buffer(buf_, MAX_TRX_UDP_SIZE), sender_ep_,
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
                                               draw = drop_dist_(rng_);
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

    asio::ip::udp::socket socket_;
    asio::ip::udp::endpoint sender_ep_;
    asio::ip::udp::endpoint dst_;
    std::mt19937 rng_;
    std::mutex rng_mtx_;
    std::uniform_real_distribution<double> rate_dist_;
    std::uniform_real_distribution<double> drop_dist_;
    uint8_t *buf_;
    std::atomic<uint64_t> dropped_;
    std::atomic<uint64_t> forwarded_;
};

/// Receiver-side state shared between the callback thread and the sender thread.
struct TestState
{
    std::mutex recv_mtx;
    std::condition_variable recv_cv;
    uint64_t send_count{0}; ///< written only by sender thread before wait
    std::atomic<uint64_t> recv_count{0};
    std::atomic<uint64_t> recv_errors{0}; ///< integrity failures
    std::atomic<uint64_t> recv_dup{0};    ///< duplicate sequence numbers
    std::vector<bool> seq_seen;           ///< indexed by seq; resized in run_test

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
        const size_t plen = size - TEST_HDR_SIZE;

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

// ---------------------------------------------------------------------------
// Shared helpers
// ---------------------------------------------------------------------------

static void print_fec_results(const TestConfig &cfg, const TestState &state, uint64_t dropped, uint64_t forwarded,
                              double elapsed_sec)
{
    auto sent = state.send_count;
    auto recvd = state.recv_count.load();
    auto errors = state.recv_errors.load();
    auto dups = state.recv_dup.load();

    double msg_loss_pct = sent > 0 ? 100.0 * (1.0 - static_cast<double>(recvd) / static_cast<double>(sent)) : 0.0;
    uint64_t total_udp = dropped + forwarded;
    double udp_loss_pct = total_udp > 0 ? 100.0 * static_cast<double>(dropped) / static_cast<double>(total_udp) : 0.0;

    std::cout << '\n';
    print_divider();
    std::cout << "  ReliableUDP Test Results\n";
    print_divider();
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "  Duration          : " << elapsed_sec << " s\n";
    std::cout << "  Payload range     : " << cfg.min_payload_bytes << " - " << cfg.max_payload_bytes << " bytes\n";
    std::cout << "  Target rate       : " << cfg.target_rate_bps / 1e6 << " Mbps\n";
    std::cout << "  Loss range        : " << cfg.loss_rate_min * 100.0 << "% - " << cfg.loss_rate_max * 100.0 << "%\n";
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
    if (fec_recovered_pct < 0.0)
        fec_recovered_pct = 0.0;
    double fec_efficiency_pct = udp_loss_pct > 0.0 ? 100.0 * fec_recovered_pct / udp_loss_pct : 100.0;
    std::cout << "  UDP pkt loss      : " << udp_loss_pct << "%\n";
    std::cout << "  Message loss      : " << msg_loss_pct << "%\n";
    std::cout << "  FEC recovered     : " << fec_recovered_pct << "% of traffic  (efficiency: " << fec_efficiency_pct
              << "%)\n";
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

// ---------------------------------------------------------------------------
// FEC loopback integration test
// ---------------------------------------------------------------------------

static void run_fec_test(const TestConfig &cfg)
{
    auto &ioc = BG_SERVICE;

    auto receiver = std::make_shared<ReliableUDP>(ioc, cfg.receiver_port);

    TestState state;
    // Pre-size the seen-bitmap generously (16-bit seq wraps at 65536)
    state.seq_seen.assign(65536, false);

    receiver->set_receive_callback([&state](const uint8_t *data, size_t size) { state.on_receive(data, size); });
    receiver->start();

    LossyProxy proxy(ioc, cfg.proxy_port, cfg.receiver_port, cfg.loss_rate_min, cfg.loss_rate_max);

    auto sender = std::make_shared<ReliableUDP>(ioc, cfg.sender_port);
    sender->start();

    if (!sender->add_destination("127.0.0.1", cfg.proxy_port))
    {
        throw std::runtime_error("add_destination failed");
    }

    std::mt19937 rng(0xDEADBEEFu);
    std::uniform_int_distribution<size_t> size_dist(cfg.min_payload_bytes, cfg.max_payload_bytes);

    const double bytes_per_sec = cfg.target_rate_bps > 0.0 ? cfg.target_rate_bps / 8.0 : 0.0;
    const auto test_start = std::chrono::steady_clock::now();
    const auto deadline = test_start + std::chrono::seconds(cfg.duration_sec);

    double bytes_budget = bytes_per_sec > 0.0 ? bytes_per_sec : 0.0; // one second of initial credit
    auto last_budget_update = test_start;

    std::vector<uint8_t> msg_buf;
    msg_buf.reserve(cfg.max_payload_bytes + TEST_HDR_SIZE);

    uint32_t seq = 0;

    while (std::chrono::steady_clock::now() < deadline)
    {
        const size_t payload_size = size_dist(rng);
        const size_t total_size = payload_size + TEST_HDR_SIZE;

        if (bytes_per_sec > 0.0)
        {
            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last_budget_update).count();
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

        msg_buf.resize(total_size);
        uint8_t *payload_ptr = msg_buf.data() + TEST_HDR_SIZE;

        // Deterministic payload: byte[i] = (seq ^ i) & 0xFF
        for (size_t i = 0; i < payload_size; ++i)
        {
            payload_ptr[i] = static_cast<uint8_t>((seq ^ static_cast<uint32_t>(i)) & 0xFFu);
        }

        TestMsgHdr hdr{};
        hdr.seq = seq;
        hdr.payload_len = static_cast<uint32_t>(payload_size);
        hdr.payload_crc = adler32_compute(payload_ptr, payload_size);
        std::memcpy(msg_buf.data(), &hdr, TEST_HDR_SIZE);

        if (sender->send(msg_buf.data(), total_size))
        {
            state.send_count++;
            seq++;
        }
    }

    {
        const auto flush_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::unique_lock<std::mutex> lk(state.recv_mtx);
        state.recv_cv.wait_until(lk, flush_deadline, [&state] { return state.recv_count.load() >= state.send_count; });
    }

    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - test_start).count();

    sender->stop();
    receiver->stop();

    print_fec_results(cfg, state, proxy.dropped(), proxy.forwarded(), elapsed);

    if (state.recv_errors.load() > 0 || state.recv_dup.load() > 0)
    {
        throw std::runtime_error("FEC loopback: integrity or duplicate check failed");
    }
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

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
    std::cout << "  Payload  : " << cfg.min_payload_bytes << " - " << cfg.max_payload_bytes << " bytes\n";
    std::cout << "  Rate     : " << std::fixed << std::setprecision(2) << cfg.target_rate_bps / 1e6 << " Mbps\n";
    std::cout << "  Loss     : " << cfg.loss_rate_min * 100.0 << "% - " << cfg.loss_rate_max * 100.0 << "%\n";
    std::cout << "  Duration : " << cfg.duration_sec << " s\n\n";
    if (int rc = run_jitter_tests())
        return rc;
    try
    {
        run_fec_test(cfg);
    }
    catch (const std::exception &e)
    {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }

    return 0;
}
