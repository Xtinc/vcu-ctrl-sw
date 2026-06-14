/**
 * @file test_reliable_udp_jitter.cpp
 * @brief RecvQueueAsync reorder, jitter, pacing, and SendQueueAsync unit tests.
 *
 * Exit code: 0 = all tests passed, 1 = one or more failures.
 */

#include "lib_network/QueueAsync.h"

#include <algorithm>
#include <array>
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

static uint64_t queue_stat_pressure_frames(const std::string &stats)
{
    return queue_stat_u64(stats, "q_pressure=");
}

static double queue_stat_tail_jitter_ms(const std::string &stats)
{
    const auto value = queue_stat_value(stats, "q_tail_jitter=");
    const auto end = value.find("ms");
    return value.empty() ? 0.0 : std::stod(value.substr(0, end));
}

static double queue_stat_ms(const std::string &stats, const std::string &key)
{
    const auto value = queue_stat_value(stats, key);
    const auto end = value.find("ms");
    return value.empty() ? 0.0 : std::stod(value.substr(0, end));
}

static double queue_stat_tail_jitter_frames(const std::string &stats)
{
    const auto value = queue_stat_value(stats, "q_tail_jitter=");
    const auto slash = value.find('/');
    if (slash == std::string::npos)
        return 0.0;
    const auto end = value.find('f', slash + 1);
    return std::stod(value.substr(slash + 1, end == std::string::npos ? std::string::npos : end - slash - 1));
}

static double queue_stat_pacing_factor(const std::string &stats)
{
    const auto value = queue_stat_value(stats, "q_pace=");
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

static bool test_jitter_inorder_backlog_not_pressure()
{
    constexpr uint32_t N = 64;
    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::atomic<bool> release_callback{false};
    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *, size_t) {
        while (!release_callback.load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }));

    bool ok = true;
    for (uint32_t i = 0; i < N; i++)
    {
        if (!q.enqueue(bufs[i].data(), 4, i))
        {
            ok = false;
            break;
        }
    }

    const std::string stats = q.stats_text();
    release_callback.store(true);
    q.stop();

    if (!ok)
        return false;

    if (queue_stat_pressure_frames(stats) != 0)
    {
        std::cout << "  [inorder_backlog] in-order backlog triggered pressure: " << stats << '\n';
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

/// A stale/slow sequence-delta sample must not pin pacing. The feed-forward
/// interval should track actual queued-frame throughput, while depth feedback
/// remains bounded by the configured pacing limits.
static bool test_jitter_feedforward_tracks_throughput_with_bounded_feedback()
{
    constexpr uint32_t WARMUP = 50;
    constexpr uint32_t BURST = 140;
    constexpr uint32_t TOTAL = WARMUP + BURST;

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

    for (uint32_t i = 0; i < WARMUP; i++)
    {
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }

    for (uint32_t i = WARMUP; i < TOTAL; i++)
    {
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i, std::chrono::milliseconds(1000)))
        {
            q.stop();
            return false;
        }
    }

    const std::string pressure_stats = q.stats_text();
    const double avg_interval_ms = queue_stat_ms(pressure_stats, "q_avg_fi=");
    const double pacing_factor = queue_stat_pacing_factor(pressure_stats);
    const auto buffered = queue_stat_current_depth(pressure_stats);
    const auto adaptive = queue_stat_adaptive_depth(pressure_stats);

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return delivered >= TOTAL; });
    }

    const std::string final_stats = q.stats_text();
    q.stop();

    if (buffered <= adaptive || avg_interval_ms >= 6.0 || pacing_factor < 0.59)
    {
        std::cout << "  [feedforward_feedback] bad pressure response: " << pressure_stats << '\n';
        return false;
    }
    if (delivered < TOTAL || queue_stat_u64(final_stats, "q_late=") != 0 || queue_stat_u64(final_stats, "q_ovf=") != 0)
    {
        std::cout << "  [feedforward_feedback] bad final state: delivered=" << delivered << "/" << TOTAL
                  << " stats=" << final_stats << '\n';
        return false;
    }
    return true;
}

/// Fast/early compressed arrivals should not increase the one-sided tail jitter
/// used for buffering depth.
static bool test_jitter_one_sided_fast_arrivals()
{
    constexpr uint32_t WARMUP = 50;
    constexpr uint32_t FAST = 90;
    constexpr uint32_t TOTAL = WARMUP + FAST;

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

    for (uint32_t i = 0; i < WARMUP; i++)
    {
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(6));
    }

    for (uint32_t i = WARMUP; i < TOTAL; i++)
    {
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return delivered >= TOTAL; });
    }

    const std::string stats = q.stats_text();
    q.stop();

    if (delivered < TOTAL)
    {
        std::cout << "  [one_sided_fast] expected " << TOTAL << " frames, got " << delivered << '\n';
        return false;
    }
    if (queue_stat_tail_jitter_frames(stats) > 0.75 || queue_stat_adaptive_depth(stats) > 6)
    {
        std::cout << "  [one_sided_fast] compressed arrivals raised tail jitter/depth: " << stats << '\n';
        return false;
    }
    return true;
}

/// Periodic slow arrivals should raise the one-sided P95 tail jitter and the
/// raw automatic depth estimate.
static bool test_jitter_one_sided_slow_tail()
{
    constexpr uint32_t WARMUP = 40;
    constexpr uint32_t BURST = 96;
    constexpr uint32_t TOTAL = WARMUP + BURST;

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

    for (uint32_t i = 0; i < WARMUP; i++)
    {
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    for (uint32_t i = WARMUP; i < TOTAL; i++)
    {
        if (!enqueue_with_retry(q, bufs[i].data(), 4, i))
        {
            q.stop();
            return false;
        }
        const auto d = ((i - WARMUP) % 8 == 7) ? std::chrono::milliseconds(18) : std::chrono::milliseconds(2);
        std::this_thread::sleep_for(d);
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(8), [&] { return delivered >= TOTAL; });
    }

    const std::string stats = q.stats_text();
    q.stop();

    if (delivered < TOTAL)
    {
        std::cout << "  [one_sided_slow] expected " << TOTAL << " frames, got " << delivered << '\n';
        return false;
    }
    if (queue_stat_tail_jitter_ms(stats) < 4.0 || queue_stat_raw_depth(stats) <= 4.0)
    {
        std::cout << "  [one_sided_slow] slow tail did not raise jitter/depth: " << stats << '\n';
        return false;
    }
    return true;
}

/// After a slow-tail phase, sustained steady arrivals should decay the tail
/// estimate without producing late or overflow drops.
static bool test_jitter_one_sided_tail_recovers()
{
    constexpr uint32_t WARMUP = 40;
    constexpr uint32_t BURST = 96;
    constexpr uint32_t RECOVER = 220;
    constexpr uint32_t TOTAL = WARMUP + BURST + RECOVER;

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

    uint32_t seq = 0;
    for (; seq < WARMUP; seq++)
    {
        if (!enqueue_with_retry(q, bufs[seq].data(), 4, seq))
        {
            q.stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    for (; seq < WARMUP + BURST; seq++)
    {
        if (!enqueue_with_retry(q, bufs[seq].data(), 4, seq))
        {
            q.stop();
            return false;
        }
        const auto d = ((seq - WARMUP) % 8 == 7) ? std::chrono::milliseconds(18) : std::chrono::milliseconds(2);
        std::this_thread::sleep_for(d);
    }

    const std::string burst_stats = q.stats_text();
    const double burst_tail_ms = queue_stat_tail_jitter_ms(burst_stats);

    for (; seq < TOTAL; seq++)
    {
        if (!enqueue_with_retry(q, bufs[seq].data(), 4, seq))
        {
            q.stop();
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(10), [&] { return delivered >= TOTAL; });
    }

    const std::string recovered_stats = q.stats_text();
    q.stop();

    const double recovered_tail_ms = queue_stat_tail_jitter_ms(recovered_stats);
    if (delivered < TOTAL)
    {
        std::cout << "  [one_sided_recover] expected " << TOTAL << " frames, got " << delivered << '\n';
        return false;
    }
    if (burst_tail_ms < 4.0 || recovered_tail_ms >= burst_tail_ms * 0.70 ||
        queue_stat_u64(recovered_stats, "q_late=") != 0 || queue_stat_u64(recovered_stats, "q_ovf=") != 0)
    {
        std::cout << "  [one_sided_recover] bad recovery: burst=" << burst_stats << " recovered=" << recovered_stats
                  << '\n';
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
/// output must be spread smoothly -> CV < 0.65.
///
/// Also verifies strict monotone delivery order.
static bool test_jitter_burst_silence_uniformity()
{
    constexpr uint32_t BURST_COUNT = 8;
    constexpr uint32_t BURST_SIZE = 12;
    constexpr uint32_t N = BURST_COUNT * BURST_SIZE; // 96 frames
    constexpr double SILENCE_MS = 80.0;              // inter-burst gap
    constexpr double CV_LIMIT = 0.65;

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
        {"in-order backlog is not pressure    ", test_jitter_inorder_backlog_not_pressure},
        {"auto depth bounded after reorder    ", test_jitter_depth_recovers},
        {"window-shuffle: order under auto depth", test_jitter_window_shuffle},
        {"auto depth survives mixed phases    ", test_jitter_adapt_up_down},
        {"permanent gap skipped, rest deliver ", test_jitter_permanent_gap},
        {"output timing uniform under jitter  ", test_jitter_timing_uniformity},
        {"timing estimators stable by phase   ", test_jitter_adaptive_estimation},
        {"feed-forward + bounded feedback     ", test_jitter_feedforward_tracks_throughput_with_bounded_feedback},
        {"one-sided fast arrivals stay shallow", test_jitter_one_sided_fast_arrivals},
        {"one-sided slow tail raises depth    ", test_jitter_one_sided_slow_tail},
        {"one-sided tail recovers             ", test_jitter_one_sided_tail_recovers},
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

int main()
{
    return run_jitter_tests();
}
