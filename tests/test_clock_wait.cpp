#include "lib_network/clock_wait.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;
using ms    = std::chrono::milliseconds;
using us    = std::chrono::microseconds;

// ── helpers ───────────────────────────────────────────────────────────────

#define RUN(name)                                          \
    do                                                     \
    {                                                      \
        std::printf("  %-45s", #name " ...");              \
        name();                                            \
        std::printf("OK\n");                               \
    } while (0)

// ── test cases ────────────────────────────────────────────────────────────

static void test_past_deadline_returns_early()
{
    ClockEntry e;
    auto past = Clock::now() - ms{1};
    assert(e.wait_until(past) == ClockEntry::Result::EARLY);
}

static void test_short_wait_returns_ok()
{
    ClockEntry e;
    auto t0 = Clock::now();
    assert(e.wait_until(t0 + us{200}) == ClockEntry::Result::OK);
    auto elapsed = std::chrono::duration_cast<us>(Clock::now() - t0).count();
    assert(elapsed >= 100);    // at least 100 µs
    assert(elapsed < 50'000);  // no more than 50 ms overshoot
}

static void test_medium_wait_returns_ok()
{
    ClockEntry e;
    auto t0 = Clock::now();
    assert(e.wait_until(t0 + ms{1}) == ClockEntry::Result::OK);
    auto elapsed = std::chrono::duration_cast<us>(Clock::now() - t0).count();
    assert(elapsed >= 500);    // at least 500 µs
    assert(elapsed < 50'000);  // less than 50 ms
}

static void test_long_wait_returns_ok()
{
    ClockEntry e;
    auto t0 = Clock::now();
    assert(e.wait_until(t0 + ms{5}) == ClockEntry::Result::OK);
    auto elapsed = std::chrono::duration_cast<ms>(Clock::now() - t0).count();
    assert(elapsed >= 2);   // at least 2 ms
    assert(elapsed < 50);   // no more than 50 ms
}

static void test_cancel_interrupts_wait()
{
    ClockEntry e;
    auto far_future = Clock::now() + std::chrono::seconds{10};
    std::thread canceller([&]() {
        std::this_thread::sleep_for(ms{10});
        e.cancel();
    });
    auto t0 = Clock::now();
    assert(e.wait_until(far_future) == ClockEntry::Result::CANCELLED);
    auto elapsed = std::chrono::duration_cast<ms>(Clock::now() - t0).count();
    assert(elapsed < 1000); // returned well before 10 s
    canceller.join();
}

static void test_reset_allows_reuse()
{
    ClockEntry e;
    e.cancel();
    assert(e.wait_until(Clock::now() + us{100}) == ClockEntry::Result::CANCELLED);
    e.reset();
    assert(e.wait_until(Clock::now() + us{500}) == ClockEntry::Result::OK);
}

static void test_multiple_waiters_all_cancelled()
{
    ClockEntry e;
    constexpr int N = 4;
    std::vector<ClockEntry::Result> results(N, ClockEntry::Result::OK);
    std::vector<std::thread> threads;
    threads.reserve(N);
    auto far_future = Clock::now() + std::chrono::seconds{10};
    for (int i = 0; i < N; ++i)
        threads.emplace_back([&, i]() { results[i] = e.wait_until(far_future); });

    std::this_thread::sleep_for(ms{20});
    e.cancel();
    for (auto& t : threads)
        t.join();

    for (int i = 0; i < N; ++i)
        assert(results[i] == ClockEntry::Result::CANCELLED);
}

// ── main ──────────────────────────────────────────────────────────────────

int main()
{
    std::printf("=== clock_wait unit tests ===\n");
    RUN(test_past_deadline_returns_early);
    RUN(test_short_wait_returns_ok);
    RUN(test_medium_wait_returns_ok);
    RUN(test_long_wait_returns_ok);
    RUN(test_cancel_interrupts_wait);
    RUN(test_reset_allows_reuse);
    RUN(test_multiple_waiters_all_cancelled);
    std::printf("All tests passed.\n");
    return 0;
}
