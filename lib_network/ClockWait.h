#ifndef CLOCK_WAIT_H
#define CLOCK_WAIT_H

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

template <size_t N> class Histogram
{
    static_assert(N > 2, "Histogram must have at least 3 buckets");

    // Collect this many raw samples before switching to EMA mode.
    static constexpr size_t BOOTSTRAP_MIN = (N < 32) ? 32 : N;

    // EMA decay factor. Effective window 1/(1-DECAY) ≈ 33 samples.
    static constexpr double DECAY = 0.97;

  public:
    Histogram() : count(0), boot_n(0), observed_min(0.0), observed_max(0.0)
    {
        bcnts.fill(0.0);
        scale.fill(0.0);
    }

    void add(double val)
    {
        if (boot_n < BOOTSTRAP_MIN)
        {
            collect_boot(val);
            return;
        }
        ema_update(val);
    }

    double quantile(double q) const
    {
        if (q < 0.0 || q > 1.0)
            return std::numeric_limits<double>::quiet_NaN();

        if (boot_n < BOOTSTRAP_MIN)
        {
            if (boot_n == 0)
                return std::numeric_limits<double>::quiet_NaN();
            std::array<double, BOOTSTRAP_MIN> s;
            std::copy(boot_vals.begin(), boot_vals.begin() + boot_n, s.begin());
            std::sort(s.begin(), s.begin() + boot_n);
            if (boot_n == 1)
                return s[0];
            const double pos = q * static_cast<double>(boot_n - 1);
            const size_t lo = static_cast<size_t>(pos);
            const size_t hi = std::min(lo + 1, boot_n - 1);
            return s[lo] + (pos - lo) * (s[hi] - s[lo]);
        }

        double cum = 0.0;
        for (size_t i = 0; i < N; i++)
        {
            const double next = cum + bcnts[i];
            if (q <= next || i == N - 1)
            {
                if (bcnts[i] < 1e-12)
                    return 0.5 * (scale[i] + scale[i + 1]);
                const double local = std::max(0.0, std::min(1.0, (q - cum) / bcnts[i]));
                return scale[i] + local * (scale[i + 1] - scale[i]);
            }
            cum = next;
        }
        return scale[N];
    }

  private:
    void collect_boot(double val)
    {
        observed_min = (boot_n == 0) ? val : std::min(observed_min, val);
        observed_max = (boot_n == 0) ? val : std::max(observed_max, val);
        boot_vals[boot_n++] = val;

        if (boot_n < BOOTSTRAP_MIN)
            return;

        build_scale(observed_min, observed_max);
        bcnts.fill(0.0);
        for (size_t i = 0; i < boot_n; i++)
            bcnts[bucket_of(boot_vals[i])] += 1.0;
        for (auto &c : bcnts)
            c /= static_cast<double>(boot_n); // normalize sum == 1.0
    }

    void ema_update(double val)
    {
        if (val < scale[0])
            scale[0] = val;
        else if (val > scale[N])
            scale[N] = val;

        for (auto &c : bcnts)
            c *= DECAY;
        bcnts[bucket_of(val)] += 1.0 - DECAY;

        if (++count % 100 == 0)
            equalize();
    }

    size_t bucket_of(double val) const
    {
        if (val <= scale[0])
            return 0;
        if (val >= scale[N])
            return N - 1;
        size_t idx = 0;
        for (size_t i = 1; i < N; i++)
        {
            if (val < scale[i])
                break;
            idx = i;
        }
        return idx;
    }

    void equalize()
    {
        // Step 1: compute new interior boundaries via a single O(N) pass.
        std::array<double, N + 1> ns;
        ns[0] = scale[0];
        ns[N] = scale[N];

        double cum = 0.0;
        size_t qi = 1; // next interior boundary index to compute
        for (size_t i = 0; i < N && qi < N; i++)
        {
            const double next = cum + bcnts[i];
            while (qi < N && static_cast<double>(qi) / N <= next)
            {
                const double target = static_cast<double>(qi) / N;
                const double local = (bcnts[i] > 1e-12) ? (target - cum) / bcnts[i] : 0.0;
                ns[qi] = scale[i] + std::max(0.0, std::min(1.0, local)) * (scale[i + 1] - scale[i]);
                qi++;
            }
            cum = next;
        }
        while (qi < N)
            ns[qi++] = scale[N]; // tail guard for near-zero trailing buckets

        scale = ns;
        bcnts.fill(1.0 / N);
    }

    void build_scale(double lo, double hi)
    {
        if (hi - lo < 1e-10)
        {
            const double pad = std::max(1e-6, std::abs(lo) * 0.01);
            lo -= pad;
            hi += pad;
        }
        const double step = (hi - lo) / N;
        for (size_t i = 0; i <= N; i++)
            scale[i] = lo + i * step;
    }

    std::array<double, N> bcnts;
    std::array<double, N + 1> scale;
    uint64_t count;
    std::array<double, BOOTSTRAP_MIN> boot_vals;
    size_t boot_n;
    double observed_min;
    double observed_max;
};

/**
 * @brief Cancellable timed wait utility, thread-safe.
 *
 * Waits until the specified time point, supports cancellation and reuse.
 * Return value (int): -1 for cancelled, 0 for normal (on time), 1 for early (target already passed).
 */
class ClockEntry
{
  public:
    using ClockTP = std::chrono::steady_clock::time_point;
    using Nanos = std::chrono::nanoseconds;

    ClockEntry();
    ClockEntry(const ClockEntry &) = delete;
    ClockEntry &operator=(const ClockEntry &) = delete;

    /**
     * @brief Block until the specified time point or cancelled.
     *
     * @param target Target time point (steady_clock::time_point)
     * @return int -1: cancelled; 0: on time; 1: early (target already passed)
     * @note For very short waits (<500us), cancellation may not take effect.
     */
    int wait_until(ClockTP target);

    /**
     * @brief Cancel the current wait.
     *
     * Thread-safe, can be called from multiple threads.
     */
    void cancel();

    /**
     * @brief Reset the cancelled state, allowing reuse of this object.
     */
    void reset();

  private:
    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::atomic<bool> m_cancelled;
};

#endif // CLOCK_WAIT_H