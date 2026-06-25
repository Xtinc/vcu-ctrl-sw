/**
 * @file queue_test.cpp
 * @brief RecvQueueAsync reorder, jitter, pacing, and SendQueueAsync unit tests.
 *
 * Exit code: 0 = all tests passed, 1 = one or more failures.
 */

#include "lib_network/QueueAsync.h"
#include "lib_network/CSVWriter.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
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
    return [callback](const std::vector<QueueFrame> &frames, bool) {
        for (const auto &frame : frames)
        {
            callback(frame.data, frame.size);
        }
        return true;
    };
}

static bool queue_adaptive_depth_is_valid(const QueueStatsSnapshot &stats)
{
    return stats.depth_target_fr >= 1 && stats.depth_target_fr <= 512 && stats.depth_raw_fr >= 0.0;
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
    q.start([&](const std::vector<QueueFrame> &frames, bool) {
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

static bool test_jitter_stats_snapshot_is_stable()
{
    std::array<uint8_t, 4> buf{};
    uint32_t value = 1;
    std::memcpy(buf.data(), &value, 4);

    RecvQueueAsync q;
    q.start(single_frame_callback([](const uint8_t *, size_t) {}));

    bool ok = true;
    ok = ok && q.enqueue(buf.data(), buf.size(), 0);
    q.enqueue(buf.data(), buf.size(), 0);

    const auto snapshot1 = q.stats_snapshot();
    const auto snapshot2 = q.stats_snapshot();
    if (snapshot1.recv != 2 || snapshot1.dup != 1 || snapshot2.recv != snapshot1.recv ||
        snapshot2.dup != snapshot1.dup)
    {
        std::cout << "  [stats_delta] cumulative snapshot changed while reading\n";
        ok = false;
    }

    if (snapshot1.buf_fr != 1 || snapshot1.depth_target_fr == 0)
    {
        std::cout << "  [stats_snapshot] unexpected queue depth\n";
        ok = false;
    }

    q.stop();
    return ok;
}

static bool test_jitter_idle_snapshot_is_stable()
{
    constexpr uint32_t N = 24;
    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; i++)
        std::memcpy(bufs[i].data(), &i, 4);

    std::mutex mtx;
    std::condition_variable cv;
    uint32_t delivered = 0;

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *, size_t) {
        std::lock_guard<std::mutex> lk(mtx);
        ++delivered;
        cv.notify_one();
    }));

    bool ok = true;
    for (uint32_t i = 0; i < N; i++)
    {
        ok = ok && enqueue_with_retry(q, bufs[i].data(), bufs[i].size(), i);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(3), [&] { return delivered >= N; });
    }

    const auto active = q.stats_snapshot();
    const auto idle = q.stats_snapshot();
    q.stop();

    if (!ok || delivered < N || active.recv == 0 || active.fi_avg_ms <= 0.0)
    {
        std::cout << "  [idle_stats] active phase did not produce input stats: delivered=" << delivered << '\n';
        return false;
    }

    if (idle.recv != active.recv || idle.dlv != active.dlv || idle.fi_avg_ms <= 0.0 ||
        idle.depth_target_fr == 0)
    {
        std::cout << "  [idle_stats] repeated snapshot changed cumulative state\n";
        return false;
    }

    return true;
}

static bool test_jitter_stop_drain_is_counted()
{
    std::array<std::array<uint8_t, 4>, 3> bufs{};
    uint64_t callback_frames = 0;

    RecvQueueAsync q;
    q.start([&](const std::vector<QueueFrame> &frames, bool) {
        callback_frames += frames.size();
        return true;
    });

    bool ok = true;
    for (uint32_t seq = 0; seq < bufs.size(); ++seq)
        ok = ok && q.enqueue(bufs[seq].data(), bufs[seq].size(), seq);

    q.stop();
    const auto stats = q.stats_snapshot();
    return ok && callback_frames == bufs.size() && stats.dlv == bufs.size();
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

    const auto stats = q.stats_snapshot();
    q.stop();

    if (stats.reorder != 0 || stats.disorder_max_fr != 0 || stats.disorder_fr != 0.0)
    {
        std::cout << "  [inorder_prefill] in-order prefill counted as reorder\n";
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

    const auto stats = q.stats_snapshot();
    release_callback.store(true);
    q.stop();

    if (!ok)
        return false;

    if (stats.pressure_fr != 0)
    {
        std::cout << "  [inorder_backlog] in-order backlog triggered pressure\n";
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
    const auto stats = q.stats_snapshot();
    if (!queue_adaptive_depth_is_valid(stats))
    {
        std::cout << "  [depth_recovers] invalid automatic target depth\n";
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
    const auto stats = q.stats_snapshot();
    if (!queue_adaptive_depth_is_valid(stats))
    {
        std::cout << "  [adapt_up_down] invalid automatic target depth\n";
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

    // The gap must recover within three estimated frame intervals, with ample
    // scheduling margin for a loaded test host.
    const size_t expected = N - 1;
    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::milliseconds(150), [&] { return received.size() >= expected; });
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

/// A temporarily missing frame that arrives inside the three-frame gap window
/// must still be delivered in order and must not increment the skip counter.
static bool test_jitter_gap_filled_before_deadline()
{
    constexpr uint32_t N = 10;
    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; ++i)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<uint32_t> received;
    std::mutex mtx;
    std::condition_variable cv;

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *data, size_t) {
        uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        std::lock_guard<std::mutex> lk(mtx);
        received.push_back(value);
        cv.notify_one();
    }));

    for (uint32_t i = 0; i < 8; ++i)
        q.enqueue(bufs[i].data(), 4, i);

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(1), [&] { return received.size() >= 8; });
    }

    q.enqueue(bufs[9].data(), 4, 9);
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    q.enqueue(bufs[8].data(), 4, 8);

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(1), [&] { return received.size() >= N; });
    }
    const auto stats = q.stats_snapshot();
    q.stop();

    if (received.size() != N || stats.skip != 0)
    {
        std::cout << "  [gap_before_deadline] bad result: received=" << received.size() << '\n';
        return false;
    }
    for (uint32_t i = 0; i < N; ++i)
        if (received[i] != i)
            return false;
    return true;
}

/// Stale cleanup must remove only the expired sequence prefix. A fresh frame
/// behind that prefix must remain queued and be delivered after one cursor jump.
static bool test_jitter_stale_prefix_preserves_fresh_tail()
{
    constexpr uint32_t N = 17;
    std::vector<std::array<uint8_t, 4>> bufs(N);
    for (uint32_t i = 0; i < N; ++i)
        std::memcpy(bufs[i].data(), &i, 4);

    std::vector<uint32_t> received;
    std::mutex mtx;
    std::condition_variable cv;
    bool first_callback_entered = false;
    bool release_first_callback = false;

    RecvQueueAsync q;
    q.start(single_frame_callback([&](const uint8_t *data, size_t) {
        uint32_t value = 0;
        std::memcpy(&value, data, sizeof(value));
        std::unique_lock<std::mutex> lk(mtx);
        received.push_back(value);
        if (value == 0)
        {
            first_callback_entered = true;
            cv.notify_all();
            cv.wait(lk, [&] { return release_first_callback; });
        }
        cv.notify_all();
    }));

    for (uint32_t i = 0; i < 8; ++i)
        q.enqueue(bufs[i].data(), 4, i);

    {
        std::unique_lock<std::mutex> lk(mtx);
        if (!cv.wait_for(lk, std::chrono::seconds(1), [&] { return first_callback_entered; }))
        {
            release_first_callback = true;
            cv.notify_all();
            lk.unlock();
            q.stop();
            return false;
        }
    }

    for (uint32_t i = 8; i < 16; ++i)
        q.enqueue(bufs[i].data(), 4, i);
    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
    q.enqueue(bufs[16].data(), 4, 16);

    {
        std::lock_guard<std::mutex> lk(mtx);
        release_first_callback = true;
    }
    cv.notify_all();

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(1), [&] { return received.size() >= 2; });
    }
    const auto stats = q.stats_snapshot();
    q.stop();

    if (received.size() != 2 || received[0] != 0 || received[1] != 16 || stats.stale != 15 || stats.drop != 15 ||
        stats.skip != 15)
    {
        std::cout << "  [stale_prefix] unexpected output/counters: received=" << received.size() << '\n';
        return false;
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

    const auto stats = q.stats_snapshot();
    if (!queue_adaptive_depth_is_valid(stats))
    {
        std::cout << "  [adaptive_estimation] invalid automatic target depth\n";
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

    const auto pressure_stats = q.stats_snapshot();
    const double avg_interval_ms = pressure_stats.fi_avg_ms;
    const double pacing_factor =
        avg_interval_ms > 0.0 ? pressure_stats.fi_out_ms / avg_interval_ms : 0.0;
    const auto buffered = pressure_stats.buf_fr;
    const auto adaptive = pressure_stats.depth_target_fr;

    {
        std::unique_lock<std::mutex> lk(mtx);
        cv.wait_for(lk, std::chrono::seconds(5), [&] { return delivered >= TOTAL; });
    }

    const auto final_stats = q.stats_snapshot();
    q.stop();

    if (buffered <= adaptive || avg_interval_ms >= 6.0 || pacing_factor < 0.59)
    {
        std::cout << "  [feedforward_feedback] bad pressure response\n";
        return false;
    }
    if (delivered < TOTAL || final_stats.late != 0 || final_stats.ovf != 0)
    {
        std::cout << "  [feedforward_feedback] bad final state: delivered=" << delivered << "/" << TOTAL << '\n';
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

    const auto stats = q.stats_snapshot();
    q.stop();

    if (delivered < TOTAL)
    {
        std::cout << "  [one_sided_fast] expected " << TOTAL << " frames, got " << delivered << '\n';
        return false;
    }
    const double tail_jitter_frames =
        stats.fi_avg_ms > 0.0 ? stats.jitter_tail_ms / stats.fi_avg_ms : 0.0;
    if (tail_jitter_frames > 0.75 || stats.depth_target_fr > 6)
    {
        std::cout << "  [one_sided_fast] compressed arrivals raised tail jitter/depth\n";
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

    const auto stats = q.stats_snapshot();
    q.stop();

    if (delivered < TOTAL)
    {
        std::cout << "  [one_sided_slow] expected " << TOTAL << " frames, got " << delivered << '\n';
        return false;
    }
    if (stats.jitter_tail_ms < 4.0 || stats.depth_raw_fr <= 4.0)
    {
        std::cout << "  [one_sided_slow] slow tail did not raise jitter/depth\n";
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

    const auto burst_stats = q.stats_snapshot();
    const double burst_tail_ms = burst_stats.jitter_tail_ms;

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

    const auto recovered_stats = q.stats_snapshot();
    q.stop();

    const double recovered_tail_ms = recovered_stats.jitter_tail_ms;
    if (delivered < TOTAL)
    {
        std::cout << "  [one_sided_recover] expected " << TOTAL << " frames, got " << delivered << '\n';
        return false;
    }
    if (burst_tail_ms < 4.0 || recovered_tail_ms >= burst_tail_ms * 0.70 || recovered_stats.late != 0 ||
        recovered_stats.ovf != 0)
    {
        std::cout << "  [one_sided_recover] bad recovery: burst=" << burst_tail_ms
                  << "ms recovered=" << recovered_tail_ms << "ms\n";
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

static bool test_qs_estimator_stable_allows_immediate()
{
    QSEstimator qs;
    const auto base = std::chrono::steady_clock::now();
    bool allowed = false;
    for (int i = 0; i <= 31; ++i)
    {
        allowed = qs.note_delivery(base + std::chrono::seconds(i), 1000.0, false);
    }

    return allowed && qs.allow_immediate;
}

static bool test_qs_estimator_continuity_break_revokes_immediate()
{
    QSEstimator qs;
    const auto base = std::chrono::steady_clock::now();
    for (int i = 0; i <= 31; ++i)
        qs.note_delivery(base + std::chrono::seconds(i), 1000.0, false);

    if (qs.note_delivery(base + std::chrono::seconds(32), 1000.0, true))
        return false;

    for (int i = 33; i < 63; ++i)
    {
        if (qs.note_delivery(base + std::chrono::seconds(i), 1000.0, false))
            return false;
    }

    return qs.note_delivery(base + std::chrono::seconds(63), 1000.0, false);
}

static bool test_qs_estimator_delivery_jitter_revokes_immediate()
{
    QSEstimator qs;
    const auto base = std::chrono::steady_clock::now();
    for (int i = 0; i <= 31; ++i)
        qs.note_delivery(base + std::chrono::seconds(i), 1000.0, false);

    return !qs.note_delivery(base + std::chrono::milliseconds(33500), 1000.0, false) &&
           !qs.allow_immediate;
}

static bool test_qs_estimator_soft_timeout_does_not_reset()
{
    QSEstimator qs;
    const auto base = std::chrono::steady_clock::now();
    for (int i = 0; i <= 31; ++i)
        qs.note_delivery(base + std::chrono::seconds(i), 1000.0, false);

    return qs.note_delivery(base + std::chrono::milliseconds(32500), 1000.0, false) && qs.allow_immediate;
}

static bool test_qs_estimator_soft_window_revokes_immediate()
{
    QSEstimator qs;
    const auto base = std::chrono::steady_clock::now();
    for (int i = 0; i <= 31; ++i)
        qs.note_delivery(base + std::chrono::seconds(i), 1000.0, false);

    if (!qs.note_delivery(base + std::chrono::milliseconds(32500), 1000.0, false))
        return false;
    if (!qs.note_delivery(base + std::chrono::milliseconds(34000), 1000.0, false))
        return false;

    return !qs.note_delivery(base + std::chrono::milliseconds(35500), 1000.0, false) &&
           !qs.allow_immediate;
}

static bool test_qs_estimator_soft_window_recovers()
{
    QSEstimator qs;
    auto now = std::chrono::steady_clock::now();
    const auto base = now;
    for (int i = 0; i <= 31; ++i)
    {
        now = base + std::chrono::seconds(i);
        qs.note_delivery(now, 1000.0, false);
    }

    now += std::chrono::milliseconds(1500);
    qs.note_delivery(now, 1000.0, false);
    now += std::chrono::milliseconds(1500);
    qs.note_delivery(now, 1000.0, false);
    now += std::chrono::milliseconds(1500);
    if (qs.note_delivery(now, 1000.0, false))
        return false;

    bool recovered = false;
    for (int i = 0; i < 70; ++i)
    {
        now += std::chrono::seconds(1);
        recovered = qs.note_delivery(now, 1000.0, false);
        if (recovered)
            break;
    }

    return recovered && qs.allow_immediate;
}

static bool test_qs_estimator_two_frame_interval_allowed()
{
    QSEstimator qs;
    const auto base = std::chrono::steady_clock::now();
    for (int i = 0; i <= 31; ++i)
        qs.note_delivery(base + std::chrono::seconds(i), 1000.0, false);

    return qs.note_delivery(base + std::chrono::seconds(33), 1000.0, false) && qs.allow_immediate;
}

static bool test_queue_stats_csv_writer_rotation()
{
    const std::string path = "queue_stats_writer_rotation_test.csv";
    for (size_t index = 0; index <= 3; ++index)
    {
        const auto candidate = index == 0 ? path : path + "." + std::to_string(index);
        std::remove(candidate.c_str());
    }

    const auto start = std::chrono::steady_clock::now();
    QueueStatsSnapshot snapshot;
    {
        NetCSVWriter writer(path, 700, 2);
        writer.start(start);
        for (uint64_t index = 0; index < 30; ++index)
        {
            snapshot.recv = index + 1;
            if (writer.on_frame(start + std::chrono::milliseconds(101 * index)))
                writer.write(snapshot);
        }
        writer.stop(start + std::chrono::milliseconds(3000), snapshot);
    }

    bool ok = true;
    std::array<uint64_t, 3> session_ids{};
    for (size_t index = 0; index <= 2; ++index)
    {
        const auto candidate = index == 0 ? path : path + "." + std::to_string(index);
        std::ifstream input(candidate);
        std::string header;
        ok = ok && static_cast<bool>(std::getline(input, header));
        ok = ok && header.find("timestamp_utc,session_id,segment_id,idle_gap_s") == 0;
        ok = ok && header.find("q_tail_jitter_ms") != std::string::npos;
        ok = ok && header.find("allow_immediate") != std::string::npos;
        ok = ok && header.find("q_fb_fi_ms") == std::string::npos;
        ok = ok && header.find("q_tail_jitter_frames") == std::string::npos;
        ok = ok && header.find("q_depth_error_frames") == std::string::npos;
        ok = ok && header.find("q_pacing_factor") == std::string::npos;
        std::string row;
        if (std::getline(input, row))
        {
            const auto first_comma = row.find(',');
            const auto second_comma = row.find(',', first_comma + 1);
            ok = ok && first_comma != std::string::npos && second_comma != std::string::npos;
            if (first_comma != std::string::npos && second_comma != std::string::npos)
            {
                ok = ok && row.size() > 27 && row[4] == '-' && row[7] == '-' && row[10] == 'T' &&
                     row[19] == '.' && row[26] == 'Z';
                session_ids[index] = static_cast<uint64_t>(std::stoull(row.substr(first_comma + 1,
                                                                                  second_comma - first_comma - 1)));
            }
        }
        else
        {
            ok = false;
        }
        std::remove(candidate.c_str());
    }
    ok = ok && session_ids[0] != 0 && session_ids[0] == session_ids[1] && session_ids[1] == session_ids[2];
    std::ifstream excess(path + ".3");
    ok = ok && !excess.good();
    std::remove((path + ".3").c_str());
    return ok;
}

static bool test_queue_stats_start_preserves_previous_session()
{
    const std::string path = "queue_stats_writer_restart_test.csv";
    for (size_t index = 0; index <= 3; ++index)
        std::remove((index == 0 ? path : path + "." + std::to_string(index)).c_str());

    const auto start = std::chrono::steady_clock::now();
    QueueStatsSnapshot snapshot;
    {
        NetCSVWriter writer(path, 0, 3);
        writer.start(start);
        snapshot.recv = 1;
        if (writer.on_frame(start))
            writer.write(snapshot);
        writer.stop(start + std::chrono::milliseconds(1), snapshot);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    {
        NetCSVWriter writer(path, 0, 3);
        writer.start(start + std::chrono::seconds(1));
        snapshot.recv = 2;
        if (writer.on_frame(start + std::chrono::seconds(1)))
            writer.write(snapshot);
        writer.stop(start + std::chrono::seconds(1) + std::chrono::milliseconds(1), snapshot);
    }

    auto read_session = [](const std::string &candidate) {
        std::ifstream input(candidate);
        std::string header;
        std::string row;
        if (!std::getline(input, header) || !std::getline(input, row))
            return uint64_t{0};
        const auto first = row.find(',');
        const auto second = row.find(',', first == std::string::npos ? first : first + 1);
        if (first == std::string::npos || second == std::string::npos)
            return uint64_t{0};
        return static_cast<uint64_t>(std::stoull(row.substr(first + 1, second - first - 1)));
    };
    const auto current_session = read_session(path);
    const auto previous_session = read_session(path + ".1");
    const bool ok = current_session != 0 && previous_session != 0 && current_session != previous_session;
    for (size_t index = 0; index <= 3; ++index)
        std::remove((index == 0 ? path : path + "." + std::to_string(index)).c_str());
    return ok;
}

static std::string csv_field(const std::string &row, size_t field_index)
{
    size_t begin = 0;
    for (size_t index = 0; index < field_index; ++index)
    {
        begin = row.find(',', begin);
        if (begin == std::string::npos)
            return {};
        ++begin;
    }
    const auto end = row.find(',', begin);
    return row.substr(begin, end == std::string::npos ? end : end - begin);
}

static bool test_queue_stats_stop_keeps_segment()
{
    const std::string path = "queue_stats_writer_stop_test.csv";
    std::remove(path.c_str());

    const auto start = std::chrono::steady_clock::now();
    QueueStatsSnapshot snapshot;
    bool throttled = false;
    {
        NetCSVWriter writer(path);
        writer.start(start);
        snapshot.recv = 1;
        if (writer.on_frame(start))
            writer.write(snapshot);
        throttled = !writer.on_frame(start + std::chrono::milliseconds(50));
        snapshot.recv = 2;
        snapshot.allow_immediate = true;
        writer.stop(start + std::chrono::seconds(6), snapshot);
    }

    std::ifstream input(path);
    std::string header;
    std::string first;
    std::string final;
    const bool rows_ok = static_cast<bool>(std::getline(input, header)) && static_cast<bool>(std::getline(input, first)) &&
                         static_cast<bool>(std::getline(input, final));
    const bool ok = throttled && rows_ok && csv_field(first, 2) == "1" && csv_field(final, 2) == "1" &&
                    csv_field(final, 3) == "0.000" && csv_field(final, 24) == "1";
    input.close();
    std::remove(path.c_str());
    return ok;
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
        {"stats snapshot is stable           ", test_jitter_stats_snapshot_is_stable},
        {"idle snapshot is stable            ", test_jitter_idle_snapshot_is_stable},
        {"stop drain updates delivery count  ", test_jitter_stop_drain_is_counted},
        {"in-order prefill is not reorder     ", test_jitter_inorder_prefill_not_reorder},
        {"in-order backlog is not pressure    ", test_jitter_inorder_backlog_not_pressure},
        {"auto depth bounded after reorder    ", test_jitter_depth_recovers},
        {"window-shuffle: order under auto depth", test_jitter_window_shuffle},
        {"auto depth survives mixed phases    ", test_jitter_adapt_up_down},
        {"permanent gap skipped, rest deliver ", test_jitter_permanent_gap},
        {"gap filled before deadline          ", test_jitter_gap_filled_before_deadline},
        {"stale prefix preserves fresh tail   ", test_jitter_stale_prefix_preserves_fresh_tail},
        {"output timing uniform under jitter  ", test_jitter_timing_uniformity},
        {"timing estimators stable by phase   ", test_jitter_adaptive_estimation},
        {"feed-forward + bounded feedback     ", test_jitter_feedforward_tracks_throughput_with_bounded_feedback},
        {"one-sided fast arrivals stay shallow", test_jitter_one_sided_fast_arrivals},
        {"one-sided slow tail raises depth    ", test_jitter_one_sided_slow_tail},
        {"one-sided tail recovers             ", test_jitter_one_sided_tail_recovers},
        {"bimodal jitter: uniform output      ", test_jitter_bimodal_jitter_uniformity},
        {"burst+silence: output smoothed      ", test_jitter_burst_silence_uniformity},
        {"large window: strict order + CV     ", test_jitter_large_window_order},
        {"QS stable allows immediate          ", test_qs_estimator_stable_allows_immediate},
        {"QS continuity break revokes         ", test_qs_estimator_continuity_break_revokes_immediate},
        {"QS delivery jitter revokes          ", test_qs_estimator_delivery_jitter_revokes_immediate},
        {"QS soft timeout is tolerated        ", test_qs_estimator_soft_timeout_does_not_reset},
        {"QS soft window revokes              ", test_qs_estimator_soft_window_revokes_immediate},
        {"QS soft window recovers             ", test_qs_estimator_soft_window_recovers},
        {"QS two-frame interval allowed       ", test_qs_estimator_two_frame_interval_allowed},
        {"queue stats CSV rotation            ", test_queue_stats_csv_writer_rotation},
        {"queue stats preserve prior session ", test_queue_stats_start_preserves_previous_session},
        {"queue stats stop keeps segment      ", test_queue_stats_stop_keeps_segment},
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
