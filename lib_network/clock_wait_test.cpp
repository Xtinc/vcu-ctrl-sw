/**
 * clock_wait_test.cpp — Accuracy and correctness tests for ClockEntry::wait_until().
 *
 * Each test case prints per-case PASS/FAIL and a final summary.
 * Exit code: 0 = all passed, 1 = one or more failures.
 *
 * Test matrix:
 *   T1  Past-target      : target already elapsed → return 1 immediately (<200 µs)
 *   T2  Immediate        : target == now()        → return 0 immediately (<200 µs)
 *   T3  Short  100 µs    : clock_nanosleep path   → p99 overshoot < 500 µs
 *   T4  Short  500 µs    : boundary of SHORT_NS   → p99 overshoot < 1 ms
 *   T5  Medium   2 ms    : cond_wait hand-off     → p99 overshoot < 2 ms
 *   T6  Frame  16.67 ms  : typical 60 fps period  → p99 overshoot < 5 ms
 *   T7  Frame  33.33 ms  : typical 30 fps period  → p99 overshoot < 5 ms
 *   T8  Cancel mid-sleep : cancel() wakes early   → returns -1, latency < 5 ms
 *   T9  Cancel before    : cancel() before wait   → returns -1 immediately
 *   T10 Reset + reuse    : reset() then wait_until → returns 0 with normal accuracy
 */

#include "clock_wait.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

using namespace std::chrono;
using ns = nanoseconds;

// ─── helpers ────────────────────────────────────────────────────────────────

static int g_pass = 0;
static int g_fail = 0;

static void report(const char *name, bool ok, const char *detail = "")
{
    if (ok)
    {
        std::printf("  [PASS] %s%s\n", name, detail);
        g_pass++;
    }
    else
    {
        std::printf("  [FAIL] %s%s\n", name, detail);
        g_fail++;
    }
}

/** Measure a single wait_until() call, return overshoot in nanoseconds. */
static int64_t measure_one(ClockEntry &ce, ns delay)
{
    auto target = steady_clock::now() + delay;
    ce.wait_until(target);
    return duration_cast<ns>(steady_clock::now() - target).count();
}

struct Stats
{
    int64_t min_ns, max_ns, p50_ns, p95_ns, p99_ns;
};

static Stats compute_stats(std::vector<int64_t> &v)
{
    std::sort(v.begin(), v.end());
    const size_t n = v.size();
    auto at = [&](double q) -> int64_t {
        size_t idx = static_cast<size_t>(q * (n - 1) + 0.5);
        if (idx >= n)
            idx = n - 1;
        return v[idx];
    };
    return {v.front(), v.back(), at(0.50), at(0.95), at(0.99)};
}

/**
 * Run ITERS warm-up iterations (not measured), then ITERS measured iterations.
 * Returns overshoot statistics in nanoseconds.
 */
static Stats run_accuracy_test(ns delay, int iters = 200, int warmup = 20)
{
    ClockEntry ce;
    for (int i = 0; i < warmup; i++)
        measure_one(ce, delay);

    std::vector<int64_t> v;
    v.reserve(static_cast<size_t>(iters));
    for (int i = 0; i < iters; i++)
        v.push_back(measure_one(ce, delay));

    return compute_stats(v);
}

static const char *fmt_us(int64_t ns_val, char *buf, size_t sz)
{
    std::snprintf(buf, sz, "%.1f µs", static_cast<double>(ns_val) / 1000.0);
    return buf;
}

// ─── individual tests ───────────────────────────────────────────────────────

static void test_past_target()
{
    std::puts("\nT1  Past-target (should return immediately with code 1)");
    ClockEntry ce;
    auto past = steady_clock::now() - milliseconds(10);
    auto t0 = steady_clock::now();
    int ret = ce.wait_until(past);
    int64_t elapsed = duration_cast<ns>(steady_clock::now() - t0).count();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "  ret=%d elapsed=%.1f µs", ret, elapsed / 1000.0);
    report("T1 past-target return code==1", ret == 1, buf);
    report("T1 past-target elapsed<200µs", elapsed < 200'000LL, buf);
}

static void test_immediate()
{
    std::puts("\nT2  Immediate (target == now)");
    ClockEntry ce;
    auto t0 = steady_clock::now();
    int ret = ce.wait_until(t0);
    int64_t elapsed = duration_cast<ns>(steady_clock::now() - t0).count();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "  ret=%d elapsed=%.1f µs", ret, elapsed / 1000.0);
    // May return 0 or 1 depending on exact timing; either is acceptable.
    report("T2 immediate elapsed<200µs", elapsed < 200'000LL, buf);
}

struct AccuracyCase
{
    const char *name;
    ns delay;
    int64_t p99_limit_ns; // PASS if p99 overshoot <= this
};

static void test_accuracy(const AccuracyCase &tc)
{
    std::printf("\n%s\n", tc.name);
    Stats s = run_accuracy_test(tc.delay, 200, 20);
    char b0[32], b1[32], b2[32], b3[32], b4[32];
    std::printf("    min=%-12s p50=%-12s p95=%-12s p99=%-12s max=%s\n",
                fmt_us(s.min_ns, b0, sizeof(b0)), fmt_us(s.p50_ns, b1, sizeof(b1)),
                fmt_us(s.p95_ns, b2, sizeof(b2)), fmt_us(s.p99_ns, b3, sizeof(b3)),
                fmt_us(s.max_ns, b4, sizeof(b4)));

    char detail[80];
    std::snprintf(detail, sizeof(detail), "  (p99=%.1f µs  limit=%.1f µs)", s.p99_ns / 1000.0,
                  tc.p99_limit_ns / 1000.0);
    report(tc.name, s.p99_ns <= tc.p99_limit_ns, detail);
}

static void test_cancel_mid_sleep()
{
    std::puts("\nT8  Cancel mid-sleep");
    ClockEntry ce;
    // Target 2 seconds in the future; cancel after 50 ms.
    auto target = steady_clock::now() + seconds(2);
    auto t0 = steady_clock::now();

    std::thread canceller([&] {
        std::this_thread::sleep_for(milliseconds(50));
        ce.cancel();
    });

    int ret = ce.wait_until(target);
    int64_t elapsed = duration_cast<ns>(steady_clock::now() - t0).count();
    canceller.join();

    char buf[64];
    std::snprintf(buf, sizeof(buf), "  ret=%d elapsed=%.1f ms", ret, elapsed / 1e6);
    report("T8 cancel returns -1", ret == -1, buf);
    // Should wake within 50ms + 5ms tolerance.
    report("T8 cancel latency < 55 ms", elapsed < 55'000'000LL, buf);
}

static void test_cancel_before_wait()
{
    std::puts("\nT9  Cancel before wait");
    ClockEntry ce;
    ce.cancel();
    auto target = steady_clock::now() + seconds(2);
    auto t0 = steady_clock::now();
    int ret = ce.wait_until(target);
    int64_t elapsed = duration_cast<ns>(steady_clock::now() - t0).count();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "  ret=%d elapsed=%.1f µs", ret, elapsed / 1000.0);
    report("T9 pre-cancelled returns -1", ret == -1, buf);
    report("T9 pre-cancelled elapsed<500µs", elapsed < 500'000LL, buf);
}

static void test_reset_reuse()
{
    std::puts("\nT10 Reset + reuse after cancel");
    ClockEntry ce;
    ce.cancel();
    ce.reset();

    // After reset, a 5 ms wait should complete normally.
    auto target = steady_clock::now() + milliseconds(5);
    auto t0 = steady_clock::now();
    int ret = ce.wait_until(target);
    int64_t elapsed = duration_cast<ns>(steady_clock::now() - t0).count();
    int64_t overshoot = duration_cast<ns>(steady_clock::now() - target).count();
    char buf[64];
    std::snprintf(buf, sizeof(buf), "  ret=%d elapsed=%.2f ms overshoot=%.1f µs", ret,
                  elapsed / 1e6, overshoot / 1000.0);
    report("T10 reset returns 0", ret == 0, buf);
    report("T10 reset overshoot<2ms", overshoot < 2'000'000LL, buf);
}

// ─── main ───────────────────────────────────────────────────────────────────

int main()
{
    std::puts("=== ClockEntry::wait_until() accuracy tests ===\n");
    std::puts("All overshoot values = actual_wake_time - target_time.");
    std::puts("Negative values mean the implementation woke early (ideal = near 0).");

    test_past_target();
    test_immediate();

    // clang-format off
    static const AccuracyCase cases[] = {
        {"T3  Short  100 µs",   microseconds(100),           500'000LL},
        {"T4  Short  500 µs",   microseconds(500),         1'000'000LL},
        {"T5  Medium   2 ms",   milliseconds(2),           2'000'000LL},
        {"T6  Frame  16.67 ms", nanoseconds(16'666'667LL), 5'000'000LL},
        {"T7  Frame  33.33 ms", nanoseconds(33'333'333LL), 5'000'000LL},
    };
    // clang-format on

    for (const auto &tc : cases)
        test_accuracy(tc);

    test_cancel_mid_sleep();
    test_cancel_before_wait();
    test_reset_reuse();

    std::printf("\n=== Summary: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail > 0 ? 1 : 0;
}
