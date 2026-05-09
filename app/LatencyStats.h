#ifndef LATENCY_STATS_H
#define LATENCY_STATS_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <deque>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_common_enc/EncChanParam.h"
#include "lib_encode/lib_encoder.h"
}

struct FrameSeiData
{
    uint64_t timestamp = 0;
    uint64_t reserved = 0;
};

using FrameSeiMap = std::unordered_map<AL_TBuffer *, FrameSeiData>;
using FrameSeiFifo = std::deque<FrameSeiData>;

template <size_t N> class Histogram
{
    static_assert(N > 2, "Histogram must have at least 3 buckets");

    // Collect this many raw samples before switching to EMA mode.
    static constexpr size_t BOOTSTRAP_MIN = (N < 32) ? 32 : N;

    // EMA decay factor. Effective window 1/(1-DECAY) = 100 samples.
    static constexpr double DECAY = 0.99;

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

class LatencyInjector
{
  public:
    LatencyInjector();
    ~LatencyInjector();
    void on_frame_submitted(AL_TBuffer *pSrcFrame);
    bool on_frame_encoded(AL_HEncoder hEnc, AL_TBuffer *pStream, AL_TBuffer *pSrcFrame);
    void on_frame_skipped(AL_TBuffer *pSrcFrame);

  private:
    bool pop_frame_data(AL_TBuffer *pSrcFrame, FrameSeiData &out_data);

  private:
    FrameSeiMap m_frames;
    std::mutex m_mutex;
};

class LatencyMeasurer
{
  public:
    LatencyMeasurer();
    ~LatencyMeasurer();
    void on_parsed_sei(bool is_prefix, int payload_type, uint8_t *payload, int payload_size);
    void on_frame_displayed(AL_TBuffer *pDisplayedFrame);

  private:
    bool take_parsed_sei_data(FrameSeiData &out_data);
    double latency_e2e() const;

  private:
    Histogram<10> m_stats;
    mutable std::mutex m_mutex;
    FrameSeiFifo m_parsed_sei_fifo;
};

#endif // LATENCY_STATS_H