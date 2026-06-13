#include "lib_network/QueueAsync.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace
{
using ClockTP = ClockEntry::ClockTP;

ClockTP advance_ms(ClockTP t, double ms)
{
    return t + std::chrono::duration_cast<ClockTP::duration>(std::chrono::duration<double, std::milli>(ms));
}

void feed_intervals(RJEstimator &estimator, const std::vector<double> &intervals_ms, uint32_t &seq, ClockTP &time)
{
    for (const double interval_ms : intervals_ms)
    {
        time = advance_ms(time, interval_ms);
        estimator.note(seq++, time, 1024);
    }
}

bool expect_near(const std::string &name, double value, double expected, double tolerance)
{
    const double error = std::abs(value - expected);
    if (error <= tolerance)
        return true;

    std::cerr << name << " expected " << expected << " +/- " << tolerance << ", got " << value << '\n';
    return false;
}

bool expect_gt(const std::string &name, double value, double threshold)
{
    if (value > threshold)
        return true;

    std::cerr << name << " expected > " << threshold << ", got " << value << '\n';
    return false;
}

bool expect_lt(const std::string &name, double value, double threshold)
{
    if (value < threshold)
        return true;

    std::cerr << name << " expected < " << threshold << ", got " << value << '\n';
    return false;
}

void print_result(const std::string &name, const RJEstimator &estimator)
{
    std::cout << std::fixed << std::setprecision(3) << name << ": interval_avg=" << estimator.interval_avg
              << "ms, jitter_avg=" << estimator.jitter_avg << "ms, jitter_tail=" << estimator.jitter_tail << "ms\n";
}

bool test_stable_interval()
{
    RJEstimator estimator;
    uint32_t seq = 0;
    ClockTP time{};

    estimator.note(seq++, time, 1024);
    feed_intervals(estimator, std::vector<double>(300, 10.0), seq, time);
    print_result("stable_10ms", estimator);

    bool ok = true;
    ok &= expect_near("stable interval_avg", estimator.interval_avg, 10.0, 0.05);
    ok &= expect_lt("stable jitter_avg", estimator.jitter_avg, 0.05);
    ok &= expect_lt("stable jitter_tail", estimator.jitter_tail, 0.05);
    return ok;
}

bool test_mixed_jitter()
{
    RJEstimator estimator;
    uint32_t seq = 0;
    ClockTP time{};

    estimator.note(seq++, time, 1024);

    std::vector<double> intervals;
    intervals.reserve(240);
    for (size_t i = 0; i < 60; ++i)
    {
        intervals.push_back(10.0);
        intervals.push_back(2.0);
        intervals.push_back(30.0);
        intervals.push_back(18.0);
    }

    feed_intervals(estimator, intervals, seq, time);
    print_result("mixed_2_to_30ms", estimator);

    bool ok = true;
    ok &= expect_gt("mixed jitter_avg", estimator.jitter_avg, 5.0);
    ok &= expect_gt("mixed jitter_tail", estimator.jitter_tail, 8.0);
    ok &= expect_gt("mixed interval_avg lower bound", estimator.interval_avg, 8.0);
    ok &= expect_lt("mixed interval_avg upper bound", estimator.interval_avg, 22.0);
    return ok;
}

bool test_large_jitter_does_not_pollute_interval()
{
    RJEstimator estimator;
    uint32_t seq = 0;
    ClockTP time{};

    estimator.note(seq++, time, 1024);
    feed_intervals(estimator, std::vector<double>(200, 10.0), seq, time);
    const double interval_before_spikes = estimator.interval_avg;

    feed_intervals(estimator, std::vector<double>(20, 100.0), seq, time);
    print_result("large_100ms_spikes", estimator);

    bool ok = true;
    ok &= expect_near("spike interval_avg", estimator.interval_avg, interval_before_spikes, 0.2);
    ok &= expect_gt("spike jitter_avg", estimator.jitter_avg, 70.0);
    ok &= expect_gt("spike jitter_tail", estimator.jitter_tail, 15.0);
    return ok;
}
} // namespace

int main()
{
    bool ok = true;
    ok &= test_stable_interval();
    ok &= test_mixed_jitter();
    ok &= test_large_jitter_does_not_pollute_interval();

    if (!ok)
    {
        std::cerr << "RJEstimator tests failed\n";
        return EXIT_FAILURE;
    }

    std::cout << "RJEstimator tests passed\n";
    return EXIT_SUCCESS;
}
