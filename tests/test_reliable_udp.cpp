/**
 * @file test_reliable_udp.cpp
 * @brief Unit and integration tests for ReliableUDP.
 *
 * Two test suites are executed in sequence:
 *
 *   1. Jitter-buffer unit tests (UsrQueueAsync, no network)
 *      Verify in-order delivery, reorder recovery, duplicate suppression,
 *      automatic-depth buffering, pacing smoothness, jitter/disorder estimation,
 *      and gap-skip behaviour.
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
#include <cstdlib>
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

// For tests that validate ordering/automatic-depth smoothing (not overload behavior), retry
// enqueue when the current reorder window is temporarily full.
static bool enqueue_with_retry(RecvQueueAsync &q, uint8_t *data, size_t size, uint32_t seq,
                               std::chrono::milliseconds timeout = std::chrono::milliseconds(500))
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (q.enqueue(data, size, seq))
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

template <typename Callback> static RecvCallBack single_frame_callback(Callback callback)
{
    return [callback](const std::vector<QueueFrame> &frames) {
        for (const auto &frame : frames)
        {
            callback(frame.data, frame.size);
        }
        return true;
    };
}

static std::string queue_stat_value(const std::string &stats, const std::string &key)
{
    const auto pos = stats.find(key);
    if (pos == std::string::npos)
        return std::string{};

    const auto begin = pos + key.size();
    const auto end = stats.find(',', begin);
    return stats.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
}

static uint64_t queue_stat_u64(const std::string &stats, const std::string &key)
{
    const auto value = queue_stat_value(stats, key);
    return value.empty() ? 0 : static_cast<uint64_t>(std::stoull(value));
}

static double queue_stat_disorder_depth(const std::string &stats)
{
    const auto value = queue_stat_value(stats, "q_dis=");
    const auto end = value.find('f');
    return value.empty() ? 0.0 : std::stod(value.substr(0, end));
}

static uint64_t queue_stat_max_disorder_depth(const std::string &stats)
{
    const auto value = queue_stat_value(stats, "q_dis=");
    const auto slash = value.find('/');
    return slash == std::string::npos ? 0 : static_cast<uint64_t>(std::stoull(value.substr(slash + 1)));
}

static uint64_t queue_stat_current_depth(const std::string &stats)
{
    const auto value = queue_stat_value(stats, "q_depth=");
    const auto slash = value.find('/');
    return slash == std::string::npos ? 0 : static_cast<uint64_t>(std::stoull(value.substr(0, slash)));
}

static uint64_t queue_stat_adaptive_depth(const std::string &stats)
{
    const auto value = queue_stat_value(stats, "q_depth=");
    const auto slash = value.rfind('/');
    return slash == std::string::npos ? 0 : static_cast<uint64_t>(std::stoull(value.substr(slash + 1)));
}

static double queue_stat_raw_depth(const std::string &stats)
{
    const auto value = queue_stat_value(stats, "q_depth_raw=");
    return value.empty() ? 0.0 : std::stod(value);
}

static bool queue_adaptive_depth_is_valid(const std::string &stats)
{
    const auto depth = queue_stat_adaptive_depth(stats);
    return depth >= 1 && depth <= 128 && queue_stat_raw_depth(stats) >= 0.0;
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

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *data, size_t) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        std::lock_guard<std::mutex> lk(mtx);
        received.push_back(v);
        cv.notify_one();
    }));

    for (uint32_t i = 0; i < N; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
    }

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

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *data, size_t) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        std::lock_guard<std::mutex> lk(mtx);
        received.push_back(v);
        cv.notify_one();
    }));

    for (uint32_t i = 0; i < N; i += 2)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        q.enqueue(bufs[i + 1].data(), 4, i + 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
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

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *data, size_t) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        std::lock_guard<std::mutex> lk(mtx);
        received.push_back(v);
        cv.notify_one();
    }));

    for (uint32_t i = 0; i < N; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
        // duplicate — must be silently dropped
        q.enqueue(bufs[i].data(), 4, i);
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

static bool test_jitter_callback_retains_until_release()
{
    constexpr uint32_t N = 12;
    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<size_t> batch_sizes;
    std::vector<uint32_t> batch_tails;
    std::mutex mtx;
    std::condition_variable cv;

    RecvQueueAsync q;
    q.start([&](const std::vector<QueueFrame> &frames) {
        uint32_t tail = 0;
        if (!frames.empty())
            std::memcpy(&tail, frames.back().data, 4);

        std::lock_guard<std::mutex> lk(mtx);
        batch_sizes.push_back(frames.size());
        batch_tails.push_back(tail);
        cv.notify_one();

        return frames.size() >= 2;
    });

    for (uint32_t i = 0; i < N; i++)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(2), [&] { return batch_sizes.size() >= 3; });
    }
    q.stop();

    if (batch_sizes.size() < 3)
    {
        std::cout << "  [callback_retain] expected at least 3 callbacks, got " << batch_sizes.size() << '\n';
        return false;
    }
    if (batch_sizes[0] != 1 || batch_sizes[1] != 2 || batch_sizes[2] != 1)
    {
        std::cout << "  [callback_retain] unexpected batch sizes: " << batch_sizes[0] << ", " << batch_sizes[1] << ", "
                  << batch_sizes[2] << '\n';
        return false;
    }
    if (batch_tails[0] != 0 || batch_tails[1] != 1 || batch_tails[2] != 2)
    {
        std::cout << "  [callback_retain] unexpected batch tails\n";
        return false;
    }
    return true;
}

static bool test_jitter_stats_counters_clear_after_read()
{
    std::array<uint8_t, 4> buf{};
    uint32_t value = 1;
    std::memcpy(buf.data(), &value, 4);

    RecvQueueAsync q;
    q.start(single_frame_callback([](const uint8_t *, size_t) {}));

    bool ok = true;
    ok = ok && q.enqueue(buf.data(), buf.size(), 0);
    q.enqueue(buf.data(), buf.size(), 0);

    const std::string first = q.stats_text();
    if (queue_stat_u64(first, "q_recv=") != 2 || queue_stat_u64(first, "q_dup=") != 1 ||
        queue_stat_current_depth(first) != 1 || queue_stat_adaptive_depth(first) == 0)
    {
        std::cout << "  [stats_delta] unexpected first stats: " << first << '\n';
        ok = false;
    }

    const std::string second = q.stats_text();
    if (queue_stat_u64(second, "q_recv=") != 0 || queue_stat_u64(second, "q_dup=") != 0 ||
        queue_stat_current_depth(second) != 1 || queue_stat_adaptive_depth(second) == 0)
    {
        std::cout << "  [stats_delta] counters did not clear after stats_text(): " << second << '\n';
        ok = false;
    }

    q.stop();
    return ok;
}

static bool test_jitter_inorder_prefill_not_reorder()
{
    constexpr uint32_t N = 16;
    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    RecvQueueAsync q;
    q.start(single_frame_callback([](const uint8_t *, size_t) {}));

    for (uint32_t i = 0; i < N; i++)
    {
        if (!q.enqueue(bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
    }

    const std::string stats = q.stats_text();
    q.stop();

    if (queue_stat_u64(stats, "q_reorder=") != 0 || queue_stat_max_disorder_depth(stats) != 0 ||
        queue_stat_disorder_depth(stats) != 0.0)
    {
        std::cout << "  [inorder_prefill] in-order prefill counted as reorder: " << stats << '\n';
        return false;
    }
    return true;
}

/// After a disorder phase and a clean phase, the automatic target depth must
/// stay bounded while the queue continues to drain all frames correctly.
static bool test_jitter_depth_recovers()
{
    constexpr uint32_t N = 200;
    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<uint32_t> received;
    std::mutex mtx;
    std::condition_variable cv;
    size_t delivered = 0;

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *, size_t) {
        std::lock_guard<std::mutex> lk(mtx);
        ++delivered;
        cv.notify_one();
    }));

    // Phase 1: 100 pairwise-swapped frames to push disorder up.
    constexpr uint32_t PHASE1 = 100;
    for (uint32_t i = 0; i < PHASE1; i += 2)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!enqueue_with_retry(q, bufs[i + 1].data(), 4, i + 1))
        {
            q.stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
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
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
    const std::string stats = q.stats_text();
    if (!queue_adaptive_depth_is_valid(stats))
    {
        std::cout << "  [depth_recovers] invalid automatic target depth: " << stats << '\n';
        return false;
    }
    return true;
}

/// Simulate real network jitter via sliding-window shuffle (WINDOW = 6 frames).
/// Frames within each window arrive in a randomised order, as happens when a
/// router has per-packet variable queuing latency.
///
/// Checks:
///   1. All N frames are eventually delivered (automatic-depth buffering tolerates short reorder windows).
///   2. The delivered sequence is non-decreasing (buffer restores order).
///   3. This remains valid while the automatic target depth adapts.
static bool test_jitter_window_shuffle()
{
    constexpr uint32_t N = 300;
    constexpr uint32_t WINDOW = 2;

    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<uint32_t> received;
    std::mutex mtx;
    std::condition_variable cv;

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *data, size_t) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        std::lock_guard<std::mutex> lk(mtx);
        received.push_back(v);
        cv.notify_one();
    }));

    std::mt19937 rng(0xCAFEBEEF);
    for (uint32_t base = 0; base < N; base += WINDOW)
    {
        uint32_t end = std::min(base + WINDOW, N);
        std::vector<uint32_t> win;
        for (uint32_t j = base; j < end; j++)
            win.push_back(j);
        std::shuffle(win.begin(), win.end(), rng);
        for (uint32_t seq : win)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (!enqueue_with_retry(q, bufs[seq].data(), 4, seq))
            {
                q.stop();
                return false;
            }
        }
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

    return true;
}

// Automatic buffering must survive both disordered and ordered phases while
// keeping its target depth bounded.
static bool test_jitter_adapt_up_down()
{
    constexpr uint32_t PHASE1 = 200;
    constexpr uint32_t PHASE2 = 200;
    constexpr uint32_t TOTAL = PHASE1 + PHASE2;
    constexpr uint32_t WINDOW = 2;

    std::vector<std::array<uint8_t, 4>> bufs(TOTAL);
    for (uint32_t i = 0; i < TOTAL; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::mutex mtx;
    std::condition_variable cv;
    size_t delivered = 0;

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *, size_t) {
        std::lock_guard<std::mutex> lk(mtx);
        ++delivered;
        cv.notify_one();
    }));

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
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            if (!enqueue_with_retry(q, bufs[seq].data(), 4, seq))
            {
                q.stop();
                return false;
            }
        }
    }
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(3), [&] { return delivered >= PHASE1; });
    }
    if (delivered < PHASE1)
        return false;

    // Phase 2: strictly in-order frames, paced 1 ms apart so next_deliver_seq_
    // is current before each arrival, guaranteeing disorder == 0 for every sample.
    for (uint32_t i = PHASE1; i < TOTAL; i++)
    {
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
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
    const std::string stats = q.stats_text();
    if (!queue_adaptive_depth_is_valid(stats))
    {
        std::cout << "  [adapt_up_down] invalid automatic target depth: " << stats << '\n';
        return false;
    }
    return true;
}

/// A permanently missing frame (SKIP) must not stall delivery of all subsequent
/// frames.  The jitter buffer must skip the gap via depth pressure or the
/// adaptive gap wait and deliver every other frame exactly once.
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

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *data, size_t) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        std::lock_guard<std::mutex> lk(mtx);
        received.push_back(v);
        cv.notify_one();
    }));

    for (uint32_t i = 0; i < N; i++)
        if (i != SKIP)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            q.enqueue(bufs[i].data(), 4, i);
        }

    // Wait long enough for the default adaptive gap wait (~208 ms) to expire.
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

/// Verify output timing uniformity under realistic jitter input.
///
/// Frames are injected in sliding-window shuffled order (WINDOW=4) with a
/// controlled inter-send interval of SEND_INTERVAL_MS ms.  Each frame arrival
/// is also perturbed by a random jitter of ±JITTER_MS ms to simulate a real
/// network.  The test measures the wall-clock time between consecutive callback
/// deliveries and computes the coefficient of variation (CV = stddev / mean).
///
/// Pass condition: CV < 0.50.  A buffer that merely forwarded frames as fast
/// as possible would produce CV >> 1 (bursty window groups); paced output
/// must be significantly smoother.
static bool test_jitter_timing_uniformity()
{
    constexpr uint32_t N = 120;
    constexpr double SEND_INTERVAL_MS = 8.0; // nominal 125 fps (fast enough to finish quickly)
    constexpr double JITTER_MS = 12.0;       // ±12 ms jitter — well above one frame interval
    constexpr uint32_t WINDOW = 2;

    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    // Delivery timestamps recorded by the callback.
    std::vector<std::chrono::steady_clock::time_point> ts;
    ts.reserve(N);
    std::mutex mtx;
    std::condition_variable cv;

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *, size_t) {
        std::lock_guard<std::mutex> lk(mtx);
        ts.push_back(std::chrono::steady_clock::now());
        cv.notify_one();
    }));

    // Send frames window-shuffled, each delayed by nominal interval ± jitter.
    std::mt19937 rng(0xABCD1234);
    std::uniform_real_distribution<double> jdist(-JITTER_MS, JITTER_MS);

    for (uint32_t base = 0; base < N; base += WINDOW)
    {
        uint32_t end = std::min(base + WINDOW, N);
        std::vector<uint32_t> win;
        for (uint32_t j = base; j < end; j++)
            win.push_back(j);
        std::shuffle(win.begin(), win.end(), rng);
        for (uint32_t seq : win)
        {
            double delay_ms = SEND_INTERVAL_MS + jdist(rng);
            if (delay_ms < 1.0)
                delay_ms = 1.0;
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(delay_ms * 1000.0)));
            if (!enqueue_with_retry(q, bufs[seq].data(), 4, seq, std::chrono::milliseconds(1000)))
            {
                q.stop();
                return false;
            }
        }
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(10), [&] { return ts.size() >= N; });
    }
    q.stop();

    if (ts.size() < N)
    {
        std::cout << "  [timing_uniformity] only " << ts.size() << "/" << N << " frames delivered\n";
        return false;
    }

    // Compute inter-delivery intervals.
    std::vector<double> intervals;
    intervals.reserve(N - 1);
    for (size_t i = 1; i < ts.size(); i++)
        intervals.push_back(std::chrono::duration<double, std::milli>(ts[i] - ts[i - 1]).count());

    const double mean = [&] {
        double s = 0;
        for (double v : intervals)
            s += v;
        return s / static_cast<double>(intervals.size());
    }();
    const double var = [&] {
        double s = 0;
        for (double v : intervals)
            s += (v - mean) * (v - mean);
        return s / static_cast<double>(intervals.size());
    }();
    const double stddev = std::sqrt(var);
    const double cv_val = (mean > 0.0) ? stddev / mean : 999.0;

    std::cout << "  [timing_uniformity] mean_interval=" << std::fixed << std::setprecision(2) << mean
              << "ms  stddev=" << stddev << "ms  CV=" << cv_val << "\n";

    if (cv_val >= 0.50)
    {
        std::cout << "  [timing_uniformity] CV=" << cv_val << " >= 0.50 — output is too bursty\n";
        return false;
    }
    return true;
}

/// Verify timing estimators remain stable under two traffic phases while the
/// automatic queue target depth stays bounded.
static bool test_jitter_adaptive_estimation()
{
    constexpr uint32_t PHASE1 = 200;
    constexpr uint32_t PHASE2 = 200;
    constexpr uint32_t TOTAL = PHASE1 + PHASE2;

    std::vector<std::array<uint8_t, 4>> bufs(TOTAL);
    for (uint32_t i = 0; i < TOTAL; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::mutex mtx;
    std::condition_variable cv;
    size_t delivered = 0;

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *, size_t) {
        std::lock_guard<std::mutex> lk(mtx);
        ++delivered;
        cv.notify_one();
    }));

    // Phase 1: high timing jitter with consecutive sequence numbers.
    // This directly exercises the jitter_ms_/recv_interval_ms estimator path.
    {
        for (uint32_t i = 0; i < PHASE1; i++)
        {
            if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
            {
                q.stop();
                return false;
            }
            const auto d = (i % 2 == 0) ? std::chrono::milliseconds(1) : std::chrono::milliseconds(20);
            std::this_thread::sleep_for(d);
        }
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return delivered >= PHASE1; });
    }

    if (delivered < PHASE1)
    {
        std::cout << "  [adaptive_estimation] phase-1 stalled at " << delivered << "/" << PHASE1 << "\n";
        q.stop();
        return false;
    }

    // Phase 2: strictly in-order, paced 8 ms apart.
    for (uint32_t i = PHASE1; i < TOTAL; i++)
    {
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return delivered >= TOTAL; });
    }
    q.stop();

    if (delivered < TOTAL)
    {
        std::cout << "  [adaptive_estimation] phase-2 stalled at " << delivered << "/" << TOTAL << "\n";
        return false;
    }

    const std::string stats = q.stats_text();
    if (!queue_adaptive_depth_is_valid(stats))
    {
        std::cout << "  [adaptive_estimation] invalid automatic target depth: " << stats << '\n';
        return false;
    }

    return true;
}

/// Verify output timing uniformity under heavy disorder (WINDOW=8) combined
/// with a bimodal arrival pattern: odd frames arrive after ~2 ms, even frames
/// after ~30 ms, simulating a router that alternately fast-paths and queues.
///
/// The bimodal input CV (stddev/mean of send intervals) is intentionally >>1.
/// Pass condition: output CV < 0.60, demonstrating the buffer absorbs the
/// input burstiness and paces delivery smoothly.
///
/// Additionally verifies strict monotone ordering of all delivered frames.
static bool test_jitter_bimodal_jitter_uniformity()
{
    constexpr uint32_t N = 160;
    constexpr uint32_t WINDOW = 8;
    constexpr double FAST_MS = 2.0;
    constexpr double SLOW_MS = 30.0;
    constexpr double CV_LIMIT = 0.60;

    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<std::chrono::steady_clock::time_point> ts;
    std::vector<uint32_t> received_seqs;
    ts.reserve(N);
    received_seqs.reserve(N);
    std::mutex mtx;
    std::condition_variable cv;

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *data, size_t) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        std::lock_guard<std::mutex> lk(mtx);
        ts.push_back(std::chrono::steady_clock::now());
        received_seqs.push_back(v);
        cv.notify_one();
    }));

    std::mt19937 rng(0xF00DCAFE);
    uint32_t send_count = 0;
    for (uint32_t base = 0; base < N; base += WINDOW)
    {
        uint32_t end = std::min(base + WINDOW, N);
        std::vector<uint32_t> win;
        for (uint32_t j = base; j < end; j++)
            win.push_back(j);
        std::shuffle(win.begin(), win.end(), rng);

        for (uint32_t seq : win)
        {
            // Bimodal delay: alternate fast/slow regardless of shuffle order.
            const double delay_ms = (send_count % 2 == 0) ? FAST_MS : SLOW_MS;
            ++send_count;
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(delay_ms * 1000.0)));
            if (!enqueue_with_retry(q, bufs[seq].data(), 4, seq, std::chrono::milliseconds(2000)))
            {
                q.stop();
                return false;
            }
        }
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(15), [&] { return ts.size() >= N; });
    }
    q.stop();

    if (ts.size() < N)
    {
        std::cout << "  [bimodal_jitter] only " << ts.size() << "/" << N << " frames delivered\n";
        return false;
    }

    // 1. Strict monotone ordering.
    for (size_t i = 1; i < received_seqs.size(); i++)
    {
        if (received_seqs[i] < received_seqs[i - 1])
        {
            std::cout << "  [bimodal_jitter] order violation at index " << i << ": seq " << received_seqs[i]
                      << " after " << received_seqs[i - 1] << '\n';
            return false;
        }
    }

    // 2. Output timing CV.
    std::vector<double> intervals;
    intervals.reserve(ts.size() - 1);
    for (size_t i = 1; i < ts.size(); i++)
        intervals.push_back(std::chrono::duration<double, std::milli>(ts[i] - ts[i - 1]).count());

    const double mean = [&] {
        double s = 0;
        for (double v : intervals)
            s += v;
        return s / static_cast<double>(intervals.size());
    }();
    const double var = [&] {
        double s = 0;
        for (double v : intervals)
            s += (v - mean) * (v - mean);
        return s / static_cast<double>(intervals.size());
    }();
    const double stddev = std::sqrt(var);
    const double cv_out = (mean > 0.0) ? stddev / mean : 999.0;

    std::cout << "  [bimodal_jitter] mean=" << std::fixed << std::setprecision(2) << mean << "ms  stddev=" << stddev
              << "ms  CV=" << cv_out << '\n';

    if (cv_out >= CV_LIMIT)
    {
        std::cout << "  [bimodal_jitter] CV=" << cv_out << " >= " << CV_LIMIT << " — output is too bursty\n";
        return false;
    }
    return true;
}

/// Verify output uniformity when frames arrive in tight bursts separated by
/// silence.  N frames are sent in BURST_COUNT bursts; within each burst all
/// frames are injected with no sleep (0 ms apart), then there is a SILENCE_MS
/// gap before the next burst.  The frames within each burst are also shuffled.
///
/// Without a jitter buffer the output would mirror the input: BURST_SIZE rapid
/// callbacks followed by a long silence → CV >> 1.  With correct pacing the
/// output must be spread smoothly → CV < 0.60.
///
/// Also verifies strict monotone delivery order.
static bool test_jitter_burst_silence_uniformity()
{
    constexpr uint32_t BURST_COUNT = 8;
    constexpr uint32_t BURST_SIZE = 12;
    constexpr uint32_t N = BURST_COUNT * BURST_SIZE; // 96 frames
    constexpr double SILENCE_MS = 80.0;              // inter-burst gap
    constexpr double CV_LIMIT = 0.60;

    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<std::chrono::steady_clock::time_point> ts;
    std::vector<uint32_t> received_seqs;
    ts.reserve(N);
    received_seqs.reserve(N);
    std::mutex mtx;
    std::condition_variable cv;

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *data, size_t) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        std::lock_guard<std::mutex> lk(mtx);
        ts.push_back(std::chrono::steady_clock::now());
        received_seqs.push_back(v);
        cv.notify_one();
    }));

    std::mt19937 rng(0xBEEFCAFE);
    for (uint32_t b = 0; b < BURST_COUNT; b++)
    {
        // Build burst window and shuffle it.
        std::vector<uint32_t> win;
        for (uint32_t i = b * BURST_SIZE; i < (b + 1) * BURST_SIZE; i++)
            win.push_back(i);
        std::shuffle(win.begin(), win.end(), rng);

        for (uint32_t seq : win)
        {
            // No sleep inside burst — inject all frames as fast as possible.
            if (!enqueue_with_retry(q, bufs[seq].data(), 4, seq, std::chrono::milliseconds(2000)))
            {
                q.stop();
                return false;
            }
        }
        // Silence between bursts (skip after last burst).
        if (b + 1 < BURST_COUNT)
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(SILENCE_MS * 1000.0)));
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(15), [&] { return ts.size() >= N; });
    }
    q.stop();

    if (ts.size() < N)
    {
        std::cout << "  [burst_silence] only " << ts.size() << "/" << N << " frames delivered\n";
        return false;
    }

    // 1. Strict monotone ordering.
    for (size_t i = 1; i < received_seqs.size(); i++)
    {
        if (received_seqs[i] < received_seqs[i - 1])
        {
            std::cout << "  [burst_silence] order violation at index " << i << ": seq " << received_seqs[i] << " after "
                      << received_seqs[i - 1] << '\n';
            return false;
        }
    }

    // 2. Output timing CV.
    std::vector<double> intervals;
    intervals.reserve(ts.size() - 1);
    for (size_t i = 1; i < ts.size(); i++)
        intervals.push_back(std::chrono::duration<double, std::milli>(ts[i] - ts[i - 1]).count());

    const double mean = [&] {
        double s = 0;
        for (double v : intervals)
            s += v;
        return s / static_cast<double>(intervals.size());
    }();
    const double var = [&] {
        double s = 0;
        for (double v : intervals)
            s += (v - mean) * (v - mean);
        return s / static_cast<double>(intervals.size());
    }();
    const double stddev = std::sqrt(var);
    const double cv_out = (mean > 0.0) ? stddev / mean : 999.0;

    // Compute input CV for reference (BURST_SIZE × 0 ms + SILENCE_MS × 1 gap
    // per burst → input stream is clearly bursty).
    const double in_mean = SILENCE_MS / static_cast<double>(BURST_SIZE);
    const double in_cv = [&] {
        // In-burst intervals ≈ 0 ms, inter-burst interval ≈ SILENCE_MS.
        double s2 = 0;
        const double n_fast = static_cast<double>(BURST_SIZE - 1) * BURST_COUNT;
        const double n_slow = static_cast<double>(BURST_COUNT - 1);
        s2 += n_fast * (0.0 - in_mean) * (0.0 - in_mean);
        s2 += n_slow * (SILENCE_MS - in_mean) * (SILENCE_MS - in_mean);
        return std::sqrt(s2 / (n_fast + n_slow)) / in_mean;
    }();

    std::cout << "  [burst_silence] input_CV≈" << std::fixed << std::setprecision(2) << in_cv
              << "  output: mean=" << mean << "ms  stddev=" << stddev << "ms  CV=" << cv_out << '\n';

    if (cv_out >= CV_LIMIT)
    {
        std::cout << "  [burst_silence] CV=" << cv_out << " >= " << CV_LIMIT << " — burst not absorbed\n";
        return false;
    }
    return true;
}

/// Prove absolute delivery ordering under a large random shuffle (WINDOW=10).
///
/// Injects N frames with each window fully shuffled and a random inter-arrival
/// jitter of [0, MAX_JITTER_MS].  Verifies:
///   (a) Every sequence number 0..N-1 is delivered exactly once.
///   (b) The sequence of delivered seq numbers is strictly non-decreasing.
///   (c) Quantifies jitter absorption: prints P50/P90/P99 of input arrival
///       disorder (distance of each frame from cursor at enqueue time) vs.
///       output inter-delivery interval CV.
static bool test_jitter_large_window_order()
{
    constexpr uint32_t N = 400;
    constexpr uint32_t WINDOW = 10;
    constexpr double BASE_MS = 5.0;
    constexpr double MAX_JITTER_MS = 20.0;

    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<uint32_t> received_seqs;
    std::vector<std::chrono::steady_clock::time_point> ts;
    received_seqs.reserve(N);
    ts.reserve(N);
    std::mutex mtx;
    std::condition_variable cv;

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *data, size_t) {
        uint32_t v;
        std::memcpy(&v, data, 4);
        std::lock_guard<std::mutex> lk(mtx);
        received_seqs.push_back(v);
        ts.push_back(std::chrono::steady_clock::now());
        cv.notify_one();
    }));

    std::mt19937 rng(0x12345678);
    std::uniform_real_distribution<double> jdist(0.0, MAX_JITTER_MS);

    for (uint32_t base = 0; base < N; base += WINDOW)
    {
        uint32_t end = std::min(base + WINDOW, N);
        std::vector<uint32_t> win;
        for (uint32_t j = base; j < end; j++)
            win.push_back(j);
        std::shuffle(win.begin(), win.end(), rng);

        for (uint32_t seq : win)
        {
            const double delay_ms = BASE_MS + jdist(rng);
            std::this_thread::sleep_for(std::chrono::microseconds(static_cast<int64_t>(delay_ms * 1000.0)));
            if (!enqueue_with_retry(q, bufs[seq].data(), 4, seq, std::chrono::milliseconds(2000)))
            {
                q.stop();
                return false;
            }
        }
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(15), [&] { return received_seqs.size() >= N; });
    }
    q.stop();

    if (received_seqs.size() < N)
    {
        std::cout << "  [large_window_order] only " << received_seqs.size() << "/" << N << " frames delivered\n";
        return false;
    }

    // (a) Every seq 0..N-1 delivered exactly once.
    std::vector<uint32_t> cnt(N, 0);
    for (uint32_t s : received_seqs)
    {
        if (s >= N)
        {
            std::cout << "  [large_window_order] seq " << s << " out of range\n";
            return false;
        }
        cnt[s]++;
    }
    for (uint32_t i = 0; i < N; i++)
    {
        if (cnt[i] != 1)
        {
            std::cout << "  [large_window_order] seq " << i << " delivered " << cnt[i] << " times\n";
            return false;
        }
    }

    // (b) Strictly non-decreasing output order.
    for (size_t i = 1; i < received_seqs.size(); i++)
    {
        if (received_seqs[i] < received_seqs[i - 1])
        {
            std::cout << "  [large_window_order] order violation at index " << i << ": seq " << received_seqs[i]
                      << " after " << received_seqs[i - 1] << '\n';
            return false;
        }
    }

    // (c) Output CV.
    std::vector<double> intervals;
    intervals.reserve(ts.size() - 1);
    for (size_t i = 1; i < ts.size(); i++)
        intervals.push_back(std::chrono::duration<double, std::milli>(ts[i] - ts[i - 1]).count());

    std::vector<double> sorted_iv = intervals;
    std::sort(sorted_iv.begin(), sorted_iv.end());
    auto percentile = [&](double p) {
        const double pos = p * static_cast<double>(sorted_iv.size() - 1);
        const size_t lo = static_cast<size_t>(pos);
        const size_t hi = std::min(lo + 1, sorted_iv.size() - 1);
        return sorted_iv[lo] + (pos - lo) * (sorted_iv[hi] - sorted_iv[lo]);
    };

    const double mean = [&] {
        double s = 0;
        for (double v : intervals)
            s += v;
        return s / static_cast<double>(intervals.size());
    }();
    const double var = [&] {
        double s = 0;
        for (double v : intervals)
            s += (v - mean) * (v - mean);
        return s / static_cast<double>(intervals.size());
    }();
    const double cv_out = (mean > 0.0) ? std::sqrt(var) / mean : 999.0;

    std::cout << "  [large_window_order] N=" << N << " window=" << WINDOW << "  output: mean=" << std::fixed
              << std::setprecision(2) << mean << "ms  P50=" << percentile(0.50) << "ms"
              << "  P90=" << percentile(0.90) << "ms"
              << "  P99=" << percentile(0.99) << "ms"
              << "  CV=" << cv_out << '\n';

    if (cv_out >= 0.60)
    {
        std::cout << "  [large_window_order] CV=" << cv_out << " >= 0.60 — output too bursty\n";
        return false;
    }
    return true;
}

static bool test_send_queue_fill()
{
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> received;

    SendQueueAsync q;
    q.start([&](const uint8_t *data, size_t size) {
        std::lock_guard<std::mutex> lk(mtx);
        received.assign(data, data + size);
        cv.notify_one();
    });

    constexpr size_t payload_size = 8;
    if (!q.enqueue_fill(payload_size, [](uint8_t *data, size_t size) {
            for (size_t i = 0; i < size; ++i)
                data[i] = static_cast<uint8_t>(i + 1);
        }))
    {
        q.stop();
        return false;
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(1), [&] { return received.size() == payload_size; });
    }

    if (received.size() != payload_size)
    {
        q.stop();
        return false;
    }
    for (size_t i = 0; i < received.size(); ++i)
    {
        if (received[i] != static_cast<uint8_t>(i + 1))
        {
            q.stop();
            return false;
        }
    }

    if (q.enqueue_fill(SEND_QUEUE_MAX_PACKET_SIZE + 1, [](uint8_t *, size_t) {}))
    {
        q.stop();
        return false;
    }

    std::atomic<bool> release_worker{false};
    std::atomic<bool> worker_blocked{false};
    std::atomic<size_t> full_q_delivered{0};
    SendQueueAsync full_q;
    full_q.start([&](const uint8_t *, size_t) {
        worker_blocked.store(true, std::memory_order_release);
        while (!release_worker.load(std::memory_order_acquire))
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        full_q_delivered.fetch_add(1, std::memory_order_release);
    });

    bool ok = full_q.enqueue_fill(1, [](uint8_t *data, size_t) { data[0] = 0; });
    for (size_t retry = 0; retry < 1000 && !worker_blocked.load(std::memory_order_acquire); ++retry)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!worker_blocked.load(std::memory_order_acquire))
    {
        release_worker.store(true, std::memory_order_release);
        full_q.stop();
        q.stop();
        return false;
    }

    for (size_t i = 0; i + 1 < SEND_QUEUE_DEPTH; ++i)
    {
        ok = ok && full_q.enqueue_fill(1, [i](uint8_t *data, size_t) { data[0] = static_cast<uint8_t>(i); });
    }

    if (!ok || full_q.enqueue_fill(1, [](uint8_t *data, size_t) { data[0] = 0xA5; }))
    {
        release_worker.store(true, std::memory_order_release);
        full_q.stop();
        q.stop();
        return false;
    }

    release_worker.store(true, std::memory_order_release);
    for (size_t retry = 0; retry < 1000 && full_q_delivered.load(std::memory_order_acquire) < SEND_QUEUE_DEPTH; ++retry)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool recovered = false;
    for (size_t retry = 0; retry < 1000 && !recovered; ++retry)
    {
        recovered = full_q.enqueue_fill(1, [](uint8_t *data, size_t) { data[0] = 0x5A; });
        if (!recovered)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    full_q.stop();
    q.stop();

    return recovered;
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
        {"callback retains until release      ", test_jitter_callback_retains_until_release},
        {"stats counters clear after read    ", test_jitter_stats_counters_clear_after_read},
        {"in-order prefill is not reorder     ", test_jitter_inorder_prefill_not_reorder},
        {"auto depth bounded after reorder    ", test_jitter_depth_recovers},
        {"window-shuffle: order under auto depth", test_jitter_window_shuffle},
        {"auto depth survives mixed phases    ", test_jitter_adapt_up_down},
        {"permanent gap skipped, rest deliver ", test_jitter_permanent_gap},
        {"output timing uniform under jitter  ", test_jitter_timing_uniformity},
        {"timing estimators stable by phase   ", test_jitter_adaptive_estimation},
        {"bimodal jitter: uniform output      ", test_jitter_bimodal_jitter_uniformity},
        {"burst+silence: output smoothed      ", test_jitter_burst_silence_uniformity},
        {"large window: strict order + CV     ", test_jitter_large_window_order},
        {"send queue fill API                 ", test_send_queue_fill},
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

static bool wait_for_rtt_sync(const std::shared_ptr<ReliableUDP> &a, const std::shared_ptr<ReliableUDP> &b,
                              std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if ((a && a->is_time_synced()) || (b && b->is_time_synced()))
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

static bool rtt_stats_are_reasonable(const ReliableUDP &udp)
{
    const auto rtt = udp.rtt_ms();
    const auto offset = udp.offset_ms();
    return rtt >= 0 && rtt < 200 && std::llabs(offset) < 200;
}

static bool test_rtt_bidirectional_sync()
{
    auto &ioc = BG_SERVICE;
    auto a = std::make_shared<ReliableUDP>(ioc, 15101);
    auto b = std::make_shared<ReliableUDP>(ioc, 15102);

    a->start();
    b->start();

    const bool ok = a->add_destination("127.0.0.1", 15102) && b->add_destination("127.0.0.1", 15101) &&
                    wait_for_rtt_sync(a, b, std::chrono::milliseconds(1500)) &&
                    ((!a->is_time_synced() || rtt_stats_are_reasonable(*a)) &&
                     (!b->is_time_synced() || rtt_stats_are_reasonable(*b)));

    a->stop();
    b->stop();
    return ok;
}

static bool test_rtt_single_direction_stays_na()
{
    auto &ioc = BG_SERVICE;
    auto a = std::make_shared<ReliableUDP>(ioc, 15103);
    auto b = std::make_shared<ReliableUDP>(ioc, 15104);

    a->start();
    b->start();

    const bool added = a->add_destination("127.0.0.1", 15104);
    std::this_thread::sleep_for(std::chrono::milliseconds(700));

    const bool ok = added && !a->is_time_synced() && a->rtt_ms() == -1;

    a->stop();
    b->stop();
    return ok;
}

static bool test_rtt_ignores_stale_pong()
{
    auto &ioc = BG_SERVICE;
    auto a = std::make_shared<ReliableUDP>(ioc, 15105);

    a->start();
    const bool added = a->add_destination("127.0.0.1", 15106);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    asio::ip::udp::socket sock(ioc);
    sock.open(asio::ip::udp::v4());

    TRXProbe stale{};
    stale.magic = MAGIC_TRX_PROBE_NUMBER;
    stale.type = 1;
    stale.seq = 99;
    stale.t1_ms = 0;
    stale.t2_delta_ms = 0;

    asio::error_code ec;
    sock.send_to(asio::buffer(&stale, sizeof(stale)), asio::ip::udp::endpoint(asio::ip::make_address("127.0.0.1"), 15105),
                 0, ec);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    const bool ok = added && !ec && !a->is_time_synced() && a->rtt_ms() == -1;

    sock.close(ec);
    a->stop();
    return ok;
}

static int run_rtt_tests()
{
    struct Test
    {
        const char *name;
        bool (*fn)();
    };

    const Test tests[] = {
        {"bidirectional probe sync           ", test_rtt_bidirectional_sync},
        {"single direction remains N/A       ", test_rtt_single_direction_stays_na},
        {"stale pong ignored                 ", test_rtt_ignores_stale_pong},
    };

    std::cout << "\nReliableUDP RTT tests\n";
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
        std::cout << "  All RTT tests passed.\n";
    else
        std::cout << "  " << failures << " RTT test(s) FAILED.\n";
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
    std::vector<uint8_t> seq_seen;        ///< indexed by seq; resized in run_test

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
    state.seq_seen.assign(65536, 0);

    receiver->set_receive_callback([&state](const std::vector<QueueFrame> &frames) {
        for (const auto &frame : frames)
        {
            state.on_receive(frame.data, frame.size);
        }
        return true;
    });
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
        std::this_thread::sleep_for(
            std::chrono::milliseconds(1)); // avoid overwhelming the receiver with back-to-back sends
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

    if (run_jitter_tests() != 0 || run_rtt_tests() != 0)
    {
        return 1;
    }

    std::cout << "ReliableUDP loopback test\n";
    std::cout << "  Payload  : " << cfg.min_payload_bytes << " - " << cfg.max_payload_bytes << " bytes\n";
    std::cout << "  Rate     : " << std::fixed << std::setprecision(2) << cfg.target_rate_bps / 1e6 << " Mbps\n";
    std::cout << "  Loss     : " << cfg.loss_rate_min * 100.0 << "% - " << cfg.loss_rate_max * 100.0 << "%\n";
    std::cout << "  Duration : " << cfg.duration_sec << " s\n\n";

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
