#include "QueueAsync.h"
#include "NetworkCfg.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

extern "C"
{
#include "lib_rtos/lib_rtos.h"
}

#if LINUX_OS_ENVIRONMENT
#include <pthread.h>
#include <sched.h>
#elif WINDOWS_OS_ENVIRONMENT
#include <windows.h>
#endif

template <typename T> static T clamp_value(const T value, const T low, const T high)
{
    return std::max(low, std::min(value, high));
}

namespace
{
constexpr const char *RECV_QUEUE_CONFIG_FILE = "config.ini";

double ema_gain(double window_frames)
{
    if (!std::isfinite(window_frames) || window_frames < 1.0)
        window_frames = 1.0;
    return 1.0 / window_frames;
}

double ema_decay(double window_frames)
{
    return 1.0 - ema_gain(window_frames);
}

double positive_or_default(double value, double fallback)
{
    return (std::isfinite(value) && value > 0.0) ? value : fallback;
}

size_t positive_or_default(size_t value, size_t fallback)
{
    return value > 0 ? value : fallback;
}

uint32_t positive_or_default(uint32_t value, uint32_t fallback)
{
    return value > 0 ? value : fallback;
}

void sanitize_mode_tuning(Tunables::UsrMode &tuning, const Tunables::UsrMode &defaults)
{
    tuning.min_depth = positive_or_default(tuning.min_depth, defaults.min_depth);
    tuning.max_depth = positive_or_default(tuning.max_depth, defaults.max_depth);
    if (tuning.max_depth < tuning.min_depth)
        tuning.max_depth = tuning.min_depth;
    tuning.initial_depth = clamp_value(positive_or_default(tuning.initial_depth, defaults.initial_depth),
                                       tuning.min_depth, tuning.max_depth);

    tuning.stale_timeout_ms = positive_or_default(tuning.stale_timeout_ms, defaults.stale_timeout_ms);
    tuning.gap_wait_frames = positive_or_default(tuning.gap_wait_frames, defaults.gap_wait_frames);
    tuning.default_frame_interval_ms =
        positive_or_default(tuning.default_frame_interval_ms, defaults.default_frame_interval_ms);
    tuning.depth_feedback_gain =
        std::isfinite(tuning.depth_feedback_gain) ? tuning.depth_feedback_gain : defaults.depth_feedback_gain;
    tuning.min_pacing_factor = positive_or_default(tuning.min_pacing_factor, defaults.min_pacing_factor);
    tuning.max_pacing_factor = positive_or_default(tuning.max_pacing_factor, defaults.max_pacing_factor);
    if (tuning.max_pacing_factor < tuning.min_pacing_factor)
        tuning.max_pacing_factor = tuning.min_pacing_factor;
    tuning.depth_margin_frames =
        std::isfinite(tuning.depth_margin_frames) ? tuning.depth_margin_frames : defaults.depth_margin_frames;
    tuning.jitter_weight = std::isfinite(tuning.jitter_weight) ? tuning.jitter_weight : defaults.jitter_weight;
    tuning.pressure_bonus_frames =
        positive_or_default(tuning.pressure_bonus_frames, defaults.pressure_bonus_frames);
}

void sanitize_recv_queue_config(Tunables &config)
{
    const Tunables defaults = Tunables::defaults();

    sanitize_mode_tuning(config.frame_complete, defaults.frame_complete);
    sanitize_mode_tuning(config.immediate, defaults.immediate);

    auto &estimator = config.estimator;
    estimator.min_frame_interval_ms =
        positive_or_default(estimator.min_frame_interval_ms, defaults.estimator.min_frame_interval_ms);
    estimator.fast_window_frames =
        positive_or_default(estimator.fast_window_frames, defaults.estimator.fast_window_frames);
    estimator.slow_window_frames =
        positive_or_default(estimator.slow_window_frames, defaults.estimator.slow_window_frames);
    estimator.tail_histogram_window_frames = positive_or_default(
        estimator.tail_histogram_window_frames, defaults.estimator.tail_histogram_window_frames);
    estimator.jitter_tail_quantile =
        std::isfinite(estimator.jitter_tail_quantile)
            ? clamp_value(estimator.jitter_tail_quantile, 0.0, 1.0)
            : defaults.estimator.jitter_tail_quantile;
    estimator.feedforward_interval_window_frames = positive_or_default(
        estimator.feedforward_interval_window_frames, defaults.estimator.feedforward_interval_window_frames);
    estimator.jitter_tail_single_factor =
        positive_or_default(estimator.jitter_tail_single_factor, defaults.estimator.jitter_tail_single_factor);
    estimator.depth_settle_epsilon_frames =
        positive_or_default(estimator.depth_settle_epsilon_frames, defaults.estimator.depth_settle_epsilon_frames);

    auto &switching = config.switching;
    switching.stable_window_seconds =
        positive_or_default(switching.stable_window_seconds, defaults.switching.stable_window_seconds);
    switching.normal_interval_factor =
        positive_or_default(switching.normal_interval_factor, defaults.switching.normal_interval_factor);
    switching.hard_timeout_interval_factor = positive_or_default(
        switching.hard_timeout_interval_factor, defaults.switching.hard_timeout_interval_factor);
    switching.soft_window_samples =
        clamp_value(positive_or_default(switching.soft_window_samples, defaults.switching.soft_window_samples),
                    static_cast<size_t>(1), QSEstimator::SOFT_WINDOW_CAPACITY);
    switching.min_window_samples =
        clamp_value(positive_or_default(switching.min_window_samples, defaults.switching.min_window_samples),
                    static_cast<size_t>(1), switching.soft_window_samples);
    switching.max_soft_timeouts =
        clamp_value(switching.max_soft_timeouts, static_cast<size_t>(0), switching.soft_window_samples);
    switching.max_absolute_interval_ms =
        positive_or_default(switching.max_absolute_interval_ms, defaults.switching.max_absolute_interval_ms);
}

bool ensure_recv_queue_defaults(INIReader &ini)
{
    bool dirty = false;
    auto ensure = [&ini, &dirty](const char *key, const char *value) {
        dirty = ini.ensure_default(key, value) || dirty;
    };

    ensure("FrameComplete.MinDepth", "1");
    ensure("FrameComplete.InitialDepth", "2");
    ensure("FrameComplete.MaxDepth", "512");
    ensure("FrameComplete.StaleTimeoutMs", "1000.0");
    ensure("FrameComplete.GapWaitFrames", "2.0");
    ensure("FrameComplete.DefaultFrameIntervalMs", "2.0");
    ensure("FrameComplete.DepthFeedbackGain", "0.08");
    ensure("FrameComplete.MinPacingFactor", "0.70");
    ensure("FrameComplete.MaxPacingFactor", "1.50");
    ensure("FrameComplete.DepthMarginFrames", "0.5");
    ensure("FrameComplete.JitterWeight", "0.5");
    ensure("FrameComplete.PressureBonusFrames", "8");

    ensure("Immediate.MinDepth", "1");
    ensure("Immediate.InitialDepth", "4");
    ensure("Immediate.MaxDepth", "512");
    ensure("Immediate.StaleTimeoutMs", "1000.0");
    ensure("Immediate.GapWaitFrames", "3.0");
    ensure("Immediate.DefaultFrameIntervalMs", "2.0");
    ensure("Immediate.DepthFeedbackGain", "0.08");
    ensure("Immediate.MinPacingFactor", "0.70");
    ensure("Immediate.MaxPacingFactor", "1.50");
    ensure("Immediate.DepthMarginFrames", "0.5");
    ensure("Immediate.JitterWeight", "1.0");
    ensure("Immediate.PressureBonusFrames", "30");

    ensure("Estimator.MinFrameIntervalMs", "1.0");
    ensure("Estimator.FastWindowFrames", "10.0");
    ensure("Estimator.SlowWindowFrames", "50.0");
    ensure("Estimator.TailHistogramWindowFrames", "1000.0");
    ensure("Estimator.JitterTailQuantile", "0.95");
    ensure("Estimator.FeedforwardIntervalWindowFrames", "16");
    ensure("Estimator.JitterTailSingleFactor", "4.0");
    ensure("Estimator.DepthSettleEpsilonFrames", "0.25");

    ensure("Switch.StableWindowSeconds", "30.0");
    ensure("Switch.NormalIntervalFactor", "1.2");
    ensure("Switch.HardTimeoutIntervalFactor", "1.6");
    ensure("Switch.SoftWindowSamples", "256");
    ensure("Switch.MinWindowSamples", "16");
    ensure("Switch.MaxSoftTimeouts", "1");
    ensure("Switch.MaxAbsoluteIntervalMs", "10000.0");

    return dirty;
}

Tunables::UsrMode read_mode_tuning(INIReader &ini, const std::string &section,
                                   const Tunables::UsrMode &defaults)
{
    const std::string prefix = section + ".";
    Tunables::UsrMode tuning;
    tuning.min_depth = ini[prefix + "MinDepth"].cast<size_t>(defaults.min_depth);
    tuning.initial_depth = ini[prefix + "InitialDepth"].cast<size_t>(defaults.initial_depth);
    tuning.max_depth = ini[prefix + "MaxDepth"].cast<size_t>(defaults.max_depth);
    tuning.stale_timeout_ms = ini[prefix + "StaleTimeoutMs"].cast<double>(defaults.stale_timeout_ms);
    tuning.gap_wait_frames = ini[prefix + "GapWaitFrames"].cast<double>(defaults.gap_wait_frames);
    tuning.default_frame_interval_ms =
        ini[prefix + "DefaultFrameIntervalMs"].cast<double>(defaults.default_frame_interval_ms);
    tuning.depth_feedback_gain = ini[prefix + "DepthFeedbackGain"].cast<double>(defaults.depth_feedback_gain);
    tuning.min_pacing_factor = ini[prefix + "MinPacingFactor"].cast<double>(defaults.min_pacing_factor);
    tuning.max_pacing_factor = ini[prefix + "MaxPacingFactor"].cast<double>(defaults.max_pacing_factor);
    tuning.depth_margin_frames = ini[prefix + "DepthMarginFrames"].cast<double>(defaults.depth_margin_frames);
    tuning.jitter_weight = ini[prefix + "JitterWeight"].cast<double>(defaults.jitter_weight);
    tuning.pressure_bonus_frames =
        ini[prefix + "PressureBonusFrames"].cast<uint32_t>(defaults.pressure_bonus_frames);
    return tuning;
}

Tunables load_recv_queue_config(INIReader &ini)
{
    const Tunables defaults = Tunables::defaults();
    Tunables config = defaults;

    config.frame_complete = read_mode_tuning(ini, "FrameComplete", defaults.frame_complete);
    config.immediate = read_mode_tuning(ini, "Immediate", defaults.immediate);

    config.estimator.min_frame_interval_ms =
        ini["Estimator.MinFrameIntervalMs"].cast<double>(defaults.estimator.min_frame_interval_ms);
    config.estimator.fast_window_frames =
        ini["Estimator.FastWindowFrames"].cast<double>(defaults.estimator.fast_window_frames);
    config.estimator.slow_window_frames =
        ini["Estimator.SlowWindowFrames"].cast<double>(defaults.estimator.slow_window_frames);
    config.estimator.tail_histogram_window_frames =
        ini["Estimator.TailHistogramWindowFrames"].cast<double>(defaults.estimator.tail_histogram_window_frames);
    config.estimator.jitter_tail_quantile =
        ini["Estimator.JitterTailQuantile"].cast<double>(defaults.estimator.jitter_tail_quantile);
    config.estimator.feedforward_interval_window_frames = ini["Estimator.FeedforwardIntervalWindowFrames"].cast<size_t>(
        defaults.estimator.feedforward_interval_window_frames);
    config.estimator.jitter_tail_single_factor =
        ini["Estimator.JitterTailSingleFactor"].cast<double>(defaults.estimator.jitter_tail_single_factor);
    config.estimator.depth_settle_epsilon_frames =
        ini["Estimator.DepthSettleEpsilonFrames"].cast<double>(defaults.estimator.depth_settle_epsilon_frames);

    config.switching.stable_window_seconds =
        ini["Switch.StableWindowSeconds"].cast<double>(defaults.switching.stable_window_seconds);
    config.switching.normal_interval_factor =
        ini["Switch.NormalIntervalFactor"].cast<double>(defaults.switching.normal_interval_factor);
    config.switching.hard_timeout_interval_factor =
        ini["Switch.HardTimeoutIntervalFactor"].cast<double>(defaults.switching.hard_timeout_interval_factor);
    config.switching.soft_window_samples =
        ini["Switch.SoftWindowSamples"].cast<size_t>(defaults.switching.soft_window_samples);
    config.switching.min_window_samples =
        ini["Switch.MinWindowSamples"].cast<size_t>(defaults.switching.min_window_samples);
    config.switching.max_soft_timeouts =
        ini["Switch.MaxSoftTimeouts"].cast<size_t>(defaults.switching.max_soft_timeouts);
    config.switching.max_absolute_interval_ms =
        ini["Switch.MaxAbsoluteIntervalMs"].cast<double>(defaults.switching.max_absolute_interval_ms);

    sanitize_recv_queue_config(config);
    return config;
}
} // namespace

void set_current_thread_scheduler_policy()
{
#if LINUX_OS_ENVIRONMENT
    auto thread = pthread_self();
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_RR) - 1;
    pthread_setschedparam(thread, SCHED_RR, &param);
    pthread_setname_np(thread, "video_thd");
#elif WINDOWS_OS_ENVIRONMENT
    auto thread = GetCurrentThread();
    SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
#endif
}

Tunables Tunables::defaults()
{
    Tunables config;
    return config;
}

SendQueueAsync::SendQueueAsync()
    : free_head_(0), free_tail_(0), free_count_(0), queue_head_(0), queue_tail_(0), queue_count_(0), running_(false)
{
    for (auto &slot : slots_)
    {
        slot.data = new uint8_t[SEND_QUEUE_MAX_PACKET_SIZE];
    }
    reset_storage_locked();
}

SendQueueAsync::~SendQueueAsync()
{
    stop();

    for (auto &slot : slots_)
    {
        delete[] slot.data;
    }
}

void SendQueueAsync::start(SendCallBack callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
        return;

    send_callback_ = std::move(callback);
    reset_storage_locked();
    running_ = true;
    thread_ = std::thread(&SendQueueAsync::worker_thread, this);
}

void SendQueueAsync::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        clear_queued_locked();
    }
    cond_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void SendQueueAsync::clear_queued_locked()
{
    while (queue_count_ > 0)
    {
        const size_t index = queued_indices_[queue_head_];
        queue_head_ = (queue_head_ + 1) % SEND_QUEUE_DEPTH;
        --queue_count_;
        slots_[index].size = 0;

        if (free_count_ < SEND_QUEUE_DEPTH)
        {
            free_indices_[free_tail_] = index;
            free_tail_ = (free_tail_ + 1) % SEND_QUEUE_DEPTH;
            ++free_count_;
        }
    }
    queue_head_ = 0;
    queue_tail_ = 0;
}

void SendQueueAsync::reset_storage_locked()
{
    queue_head_ = 0;
    queue_tail_ = 0;
    queue_count_ = 0;
    free_head_ = 0;
    free_tail_ = 0;
    free_count_ = SEND_QUEUE_DEPTH;

    for (size_t i = 0; i < SEND_QUEUE_DEPTH; ++i)
    {
        free_indices_[i] = i;
    }
}

bool SendQueueAsync::enqueue(const uint8_t *data, size_t size)
{
    if (!data || size == 0 || size > SEND_QUEUE_MAX_PACKET_SIZE)
        return false;

    return enqueue_fill(size, [data](uint8_t *dst, size_t dst_size) { std::memcpy(dst, data, dst_size); });
}

bool SendQueueAsync::enqueue_fill(size_t size, FillCallback callback)
{
    if (size == 0 || size > SEND_QUEUE_MAX_PACKET_SIZE || !callback)
        return false;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_)
        return false;

    if (free_count_ == 0)
    {
        VIDEO_ERROR_PRINT("[SendQueue] queue overflow, dropping packet size=%zu depth=%zu", size, SEND_QUEUE_DEPTH);
        return false;
    }

    const size_t index = free_indices_[free_head_];
    free_head_ = (free_head_ + 1) % SEND_QUEUE_DEPTH;
    --free_count_;

    try
    {
        callback(slots_[index].data, size);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("[SendQueue] fill callback threw exception: %s", e.what());
        free_head_ = (free_head_ + SEND_QUEUE_DEPTH - 1) % SEND_QUEUE_DEPTH;
        free_indices_[free_head_] = index;
        ++free_count_;
        return false;
    }
    catch (...)
    {
        VIDEO_ERROR_PRINT("[SendQueue] fill callback threw unknown exception");
        free_head_ = (free_head_ + SEND_QUEUE_DEPTH - 1) % SEND_QUEUE_DEPTH;
        free_indices_[free_head_] = index;
        ++free_count_;
        return false;
    }

    slots_[index].size = size;
    queued_indices_[queue_tail_] = index;
    queue_tail_ = (queue_tail_ + 1) % SEND_QUEUE_DEPTH;
    ++queue_count_;

    cond_.notify_one();
    return true;
}

void SendQueueAsync::worker_thread()
{
    set_current_thread_scheduler_policy();

    while (true)
    {
        size_t index = 0;
        size_t size = 0;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cond_.wait(lock, [this] { return !running_ || queue_count_ > 0; });

            if (!running_)
                break;

            index = queued_indices_[queue_head_];
            size = slots_[index].size;
            queue_head_ = (queue_head_ + 1) % SEND_QUEUE_DEPTH;
            --queue_count_;
        }

        try
        {
            if (send_callback_)
                send_callback_(slots_[index].data, size);
        }
        catch (const std::exception &e)
        {
            VIDEO_ERROR_PRINT("[SendQueue] send callback threw exception: %s", e.what());
        }
        catch (...)
        {
            VIDEO_ERROR_PRINT("[SendQueue] send callback threw unknown exception");
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (free_count_ < SEND_QUEUE_DEPTH)
            {
                slots_[index].size = 0;
                free_indices_[free_tail_] = index;
                free_tail_ = (free_tail_ + 1) % SEND_QUEUE_DEPTH;
                ++free_count_;
            }
        }
    }
}

RJEstimator::RJEstimator()
    : interval_avg(0.0), interval_short_avg(0.0), jitter_avg(0.0), jitter_tail(0.0), has_reference(false),
      has_interval(false), last_seq(0), last_time(ClockTP{}), window_samples(0), window_start(ClockTP{}),
      estimator_tuning(Tunables::defaults().estimator),
      jitter_hist(ema_decay(estimator_tuning.tail_histogram_window_frames))
{
}

RJEstimator::~RJEstimator() = default;

void RJEstimator::configure(const Tunables::Estimator &tuning)
{
    estimator_tuning = tuning;
    jitter_hist = Histogram<20>(ema_decay(estimator_tuning.tail_histogram_window_frames));
    reset();
}

void RJEstimator::reset()
{
    has_reference = false;
    has_interval = false;
    last_seq = 0;
    last_time = ClockTP{};
    interval_avg = 0.0;
    interval_short_avg = 0.0;
    jitter_avg = 0.0;
    jitter_tail = 0.0;
    window_samples = 0;
    window_start = ClockTP{};
    jitter_hist.reset();
}

void RJEstimator::note(uint32_t abs_seq, ClockEntry::ClockTP arrival, size_t max_seq_delta)
{
    if (window_samples == 0)
        window_start = arrival;
    if (++window_samples >= estimator_tuning.feedforward_interval_window_frames)
    {
        const double elapsed_ms = std::chrono::duration<double, std::milli>(arrival - window_start).count();
        if (elapsed_ms > 0.0)
        {
            const double sample_interval =
                (std::max)(estimator_tuning.min_frame_interval_ms,
                           elapsed_ms / static_cast<double>(window_samples - 1));
            interval_short_avg = sample_interval;
            const double gain = has_interval ? ema_gain(estimator_tuning.fast_window_frames) : 1.0;
            interval_avg += gain * (sample_interval - interval_avg);
            has_interval = true;
        }
        window_start = arrival;
        window_samples = 1;
    }

    if (!has_reference)
    {
        has_reference = true;
        last_seq = abs_seq;
        last_time = arrival;
        return;
    }

    const int64_t seq_delta = static_cast<int64_t>(abs_seq) - static_cast<int64_t>(last_seq);
    if (seq_delta <= 0)
        return;

    auto sample_valid = false;
    auto sample_interval = 0.0;
    if (seq_delta <= static_cast<int64_t>(max_seq_delta))
    {
        const auto diff = std::chrono::duration<double, std::milli>(arrival - last_time).count();
        if (diff > 0.0 && diff < 1000.0 * estimator_tuning.min_frame_interval_ms)
        {
            sample_interval = diff / static_cast<double>(seq_delta);
            sample_valid = sample_interval >= estimator_tuning.min_frame_interval_ms;
        }
    }

    if (sample_valid && has_interval)
    {
        const double late_sample_ms = (std::max)(0.0, sample_interval - interval_avg);
        const double deviation = std::abs(sample_interval - interval_avg);
        jitter_hist.add(late_sample_ms);
        jitter_avg += ema_gain(estimator_tuning.fast_window_frames) * (deviation - jitter_avg);

        const double q95_ms = jitter_hist.quantile(estimator_tuning.jitter_tail_quantile);
        const double tail_sample_ms = std::isfinite(q95_ms) ? (std::max)(q95_ms, late_sample_ms) : late_sample_ms;
        if (tail_sample_ms >= jitter_tail)
            jitter_tail += (std::min)(tail_sample_ms - jitter_tail,
                                      estimator_tuning.jitter_tail_single_factor * interval_avg);
        else
            jitter_tail += ema_gain(estimator_tuning.slow_window_frames) * (tail_sample_ms - jitter_tail);
    }

    last_seq = abs_seq;
    last_time = arrival;
}

ROEstimator::ROEstimator()
    : reorder_cnt(0), max_disorder_depth(0), depth_frames(0.0), guard_depth_frames(0.0), has_reference(false),
      highest_seq(0), estimator_tuning(Tunables::defaults().estimator)
{
}

ROEstimator::~ROEstimator() = default;

void ROEstimator::configure(const Tunables::Estimator &tuning)
{
    estimator_tuning = tuning;
    reset();
}

void ROEstimator::reset()
{
    has_reference = false;
    highest_seq = 0;
    reorder_cnt = 0;
    max_disorder_depth = 0;
    depth_frames = 0.0;
    guard_depth_frames = 0.0;
}

void ROEstimator::note(uint32_t abs_seq, size_t max_seq_delta)
{
    if (!has_reference)
    {
        has_reference = true;
        highest_seq = abs_seq;
        return;
    }

    const int64_t seq_delta = static_cast<int64_t>(abs_seq) - static_cast<int64_t>(highest_seq);
    if (seq_delta > 0)
    {
        const double forward_gap = static_cast<double>(seq_delta - 1);
        highest_seq = abs_seq;
        depth_frames *= ema_decay(estimator_tuning.slow_window_frames);
        guard_depth_frames *= ema_decay(estimator_tuning.slow_window_frames);
        guard_depth_frames = (std::max)(guard_depth_frames, forward_gap);
        return;
    }

    if (seq_delta == 0)
        return;

    const auto disorder_depth = static_cast<uint32_t>(-seq_delta);
    if (disorder_depth > max_seq_delta)
        return;

    const double sample = static_cast<double>(disorder_depth);
    ++reorder_cnt;
    max_disorder_depth = (std::max)(max_disorder_depth, disorder_depth);
    depth_frames += ema_gain(estimator_tuning.fast_window_frames) * (sample - depth_frames);
    guard_depth_frames = (std::max)(guard_depth_frames, sample);
}

void QSEstimator::configure(const Tunables &config)
{
    estimator_tuning_ = config.estimator;
    switching_tuning_ = config.switching;
    reset();
}

void QSEstimator::reset()
{
    allow_immediate = false;
    has_last_delivery_ = false;
    last_delivery_time_ = ClockTP{};
    stable_since_ = ClockTP{};
    soft_timeout_window_.fill(false);
    soft_window_pos_ = 0;
    soft_window_count_ = 0;
    soft_timeout_count_ = 0;
}

bool QSEstimator::note_delivery(ClockTP now, double expected_interval_ms, bool continuity_broken)
{
    expected_interval_ms = (std::max)(estimator_tuning_.min_frame_interval_ms, expected_interval_ms);

    if (continuity_broken)
    {
        reset();
        return allow_immediate;
    }

    if (!has_last_delivery_)
    {
        has_last_delivery_ = true;
        last_delivery_time_ = now;
        stable_since_ = now;
        allow_immediate = false;
        return allow_immediate;
    }

    const double actual_interval_ms = std::chrono::duration<double, std::milli>(now - last_delivery_time_).count();
    if (!std::isfinite(actual_interval_ms) || actual_interval_ms < 0.0 ||
        actual_interval_ms > switching_tuning_.max_absolute_interval_ms ||
        actual_interval_ms > switching_tuning_.hard_timeout_interval_factor * expected_interval_ms)
    {
        reset();
        return allow_immediate;
    }

    last_delivery_time_ = now;

    const bool soft_timeout = actual_interval_ms > switching_tuning_.normal_interval_factor * expected_interval_ms;
    if (soft_window_count_ == switching_tuning_.soft_window_samples)
    {
        if (soft_timeout_window_[soft_window_pos_])
            --soft_timeout_count_;
    }
    else
    {
        ++soft_window_count_;
    }

    soft_timeout_window_[soft_window_pos_] = soft_timeout;
    if (soft_timeout)
        ++soft_timeout_count_;
    soft_window_pos_ = (soft_window_pos_ + 1) % switching_tuning_.soft_window_samples;

    const double stable_seconds = std::chrono::duration<double>(now - stable_since_).count();
    allow_immediate = stable_seconds >= switching_tuning_.stable_window_seconds &&
                      soft_window_count_ >= switching_tuning_.min_window_samples &&
                      soft_timeout_count_ <= switching_tuning_.max_soft_timeouts;

    return allow_immediate;
}

void RecvQueueAsync::BFController::reset(size_t initial_depth)
{
    adaptive_depth = initial_depth;
    decay_depth_frames = static_cast<double>(initial_depth);
    raw_depth_frames = static_cast<double>(initial_depth);
    pressure_frames_remaining = 0;
}

void RecvQueueAsync::BFController::sanitize(const RecvQueueAsync::UsrMode &tuning)
{
    adaptive_depth =
        clamp_value(adaptive_depth == 0 ? tuning.min_depth : adaptive_depth, tuning.min_depth, tuning.max_depth);
    if (decay_depth_frames <= 0.0)
        decay_depth_frames = static_cast<double>(adaptive_depth);
    decay_depth_frames =
        clamp_value(decay_depth_frames, static_cast<double>(tuning.min_depth), static_cast<double>(tuning.max_depth));
    raw_depth_frames = (std::max)(raw_depth_frames, static_cast<double>(tuning.min_depth));
}

void RecvQueueAsync::BFController::trigger_pressure(const RecvQueueAsync::UsrMode &tuning)
{
    pressure_frames_remaining = (std::max)(pressure_frames_remaining, tuning.pressure_bonus_frames);
}

void RecvQueueAsync::BFController::consume_pressure()
{
    if (pressure_frames_remaining > 0)
        --pressure_frames_remaining;
}

void RecvQueueAsync::BFController::note(const RecvQueueAsync::UsrMode &tuning,
                                        const Tunables::Estimator &estimator_tuning,
                                        double reorder_frames, double jitter_frames)
{
    const double pressure_frames = pressure_frames_remaining > 0 ? 1.0 : 0.0;
    raw_depth_frames =
        reorder_frames + tuning.jitter_weight * jitter_frames + tuning.depth_margin_frames + pressure_frames;

    const size_t target_depth =
        clamp_value(static_cast<size_t>(std::ceil(raw_depth_frames)), tuning.min_depth, tuning.max_depth);

    if (target_depth >= adaptive_depth)
    {
        adaptive_depth = target_depth;
        decay_depth_frames = static_cast<double>(target_depth);
        return;
    }

    decay_depth_frames +=
        ema_gain(estimator_tuning.slow_window_frames) * (static_cast<double>(target_depth) - decay_depth_frames);
    decay_depth_frames =
        clamp_value(decay_depth_frames, static_cast<double>(tuning.min_depth), static_cast<double>(tuning.max_depth));
    if (decay_depth_frames - static_cast<double>(target_depth) <= estimator_tuning.depth_settle_epsilon_frames)
        decay_depth_frames = static_cast<double>(target_depth);

    adaptive_depth = clamp_value(static_cast<size_t>(std::ceil(decay_depth_frames)), target_depth, tuning.max_depth);
}

RecvQueueAsync::RecvQueueAsync()
    : config_(Tunables::defaults()), frame_pool_("recv_queue_frame", 64), running_(false), primed_(false),
      expected_seq_(0)
{
    apply_estimator_tuning_locked();
    sanitize_tuning_locked();
}

RecvQueueAsync::~RecvQueueAsync()
{
    stop();
}

void RecvQueueAsync::sanitize_tuning_locked()
{
    sanitize_recv_queue_config(config_);
    buffer_ctl_.sanitize(active_tuning_locked());
}

void RecvQueueAsync::apply_estimator_tuning_locked()
{
    arrival_est_.configure(config_.estimator);
    reorder_est_.configure(config_.estimator);
    qs_est_.configure(config_);
}

void RecvQueueAsync::load_config_locked()
{
    INIReader ini(RECV_QUEUE_CONFIG_FILE);
    const bool dirty = ensure_recv_queue_defaults(ini);
    if (dirty && !ini.save())
    {
        VIDEO_ERROR_PRINT("Failed to save default receive queue config: %s", RECV_QUEUE_CONFIG_FILE);
    }

    config_ = load_recv_queue_config(ini);
    apply_estimator_tuning_locked();
    sanitize_tuning_locked();
}

const RecvQueueAsync::UsrMode &RecvQueueAsync::active_tuning_locked() const
{
    return qs_est_.allow_immediate ? config_.immediate : config_.frame_complete;
}

void RecvQueueAsync::reset_state_locked()
{
    clear_buffered_frames_locked();
    clear_delivered_frames_locked();
    primed_ = false;
    expected_seq_ = 0;
    next_delivery_time_ = ClockTP{};
    gap_start_time_ = ClockTP{};
    gap_deadline_ = ClockTP{};

    arrival_est_.reset();
    reorder_est_.reset();
    qs_est_.reset();
    buffer_ctl_.reset(active_tuning_locked().initial_depth);
    qs_continuity_broken_ = false;
    recv_count_ = 0;
    deliver_count_ = 0;
    skip_count_ = 0;
    drop_count_ = 0;
    duplicate_count_ = 0;
    late_count_ = 0;
    stale_count_ = 0;
    overflow_count_ = 0;
}

void RecvQueueAsync::clear_buffered_frames_locked()
{
    for (auto &frame : buffered_frames_)
    {
        frame_pool_.deallocate(frame.data);
    }
    buffered_frames_.clear();
}

void RecvQueueAsync::clear_delivered_frames_locked()
{
    for (auto &frame : delivered_frames_)
    {
        frame_pool_.deallocate(frame.data);
    }
    delivered_frames_.clear();
}

void RecvQueueAsync::start(RecvCallBack callback)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (running_)
        return;

    load_config_locked();
    receive_callback_ = std::move(callback);
    reset_state_locked();
    running_ = true;
    thread_ = std::thread(&RecvQueueAsync::worker_thread, this);
}

void RecvQueueAsync::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    cond_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void RecvQueueAsync::reset()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_state_locked();
    }
    cond_.notify_all();
}

void RecvQueueAsync::update_adaptive_depth_locked()
{
    const auto &tuning = active_tuning_locked();
    buffer_ctl_.note(tuning, config_.estimator, (std::max)(reorder_est_.depth_frames, reorder_est_.guard_depth_frames),
                     arrival_est_.jitter_tail / estimated_interval_ms_locked());
}

double RecvQueueAsync::estimated_interval_ms_locked() const
{
    if (arrival_est_.interval_avg >= config_.estimator.min_frame_interval_ms)
        return arrival_est_.interval_avg;

    if (buffered_frames_.size() >= 2)
    {
        const auto &first = buffered_frames_.front();
        const auto &last = buffered_frames_.back();
        const double span_ms = std::chrono::duration<double, std::milli>(last.arrival - first.arrival).count() /
                               static_cast<double>(buffered_frames_.size() - 1);
        if (span_ms >= config_.estimator.min_frame_interval_ms)
            return span_ms;
    }

    return active_tuning_locked().default_frame_interval_ms;
}

double RecvQueueAsync::compute_delivery_interval_locked() const
{
    const auto &tuning = active_tuning_locked();
    const double base_interval_ms = estimated_interval_ms_locked();
    const double depth_error =
        static_cast<double>(buffered_frames_.size()) - static_cast<double>(buffer_ctl_.adaptive_depth);
    const double feedback_scale = 1.0 + tuning.depth_feedback_gain * depth_error;
    const double pacing_factor =
        feedback_scale <= 0.0 ? tuning.max_pacing_factor
                              : clamp_value(1.0 / feedback_scale, tuning.min_pacing_factor, tuning.max_pacing_factor);
    return (std::max)(config_.estimator.min_frame_interval_ms, base_interval_ms * pacing_factor);
}

void RecvQueueAsync::drop_frame_locked(std::list<BufferedFrame>::iterator it)
{
    uint8_t *data = it->data;
    buffered_frames_.erase(it);
    frame_pool_.deallocate(data);
}

void RecvQueueAsync::purge_stale_locked(ClockTP now)
{
    if (!primed_ || buffered_frames_.empty())
        return;

    const auto cutoff = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double, std::milli>(active_tuning_locked().stale_timeout_ms));
    if (buffered_frames_.front().arrival >= cutoff)
        return;

    // const size_t depth_before = buffered_frames_.size();
    // const uint32_t expected_before = expected_seq_;
    const uint32_t first_stale_seq = buffered_frames_.front().seq;
    uint32_t last_stale_seq = first_stale_seq;
    uint64_t stale_count = 0;
    double max_age_ms = 0.0;

    // Only remove the stale sequence prefix. A later sequence may have an
    // older arrival timestamp due to reassembly order; scanning the whole list
    // would create holes ahead of the playback cursor.
    while (!buffered_frames_.empty() && buffered_frames_.front().arrival < cutoff)
    {
        const auto &frame = buffered_frames_.front();
        last_stale_seq = frame.seq;
        max_age_ms = (std::max)(max_age_ms, std::chrono::duration<double, std::milli>(now - frame.arrival).count());
        drop_frame_locked(buffered_frames_.begin());
        ++stale_count;
    }

    drop_count_ += stale_count;
    stale_count_ += stale_count;

    const uint32_t resume_seq = buffered_frames_.empty() ? last_stale_seq + 1 : buffered_frames_.front().seq;
    const uint64_t skipped = resume_seq > expected_seq_ ? static_cast<uint64_t>(resume_seq - expected_seq_) : 0;
    skip_count_ += skipped;
    qs_continuity_broken_ = true;

    if (resume_seq > expected_seq_)
        expected_seq_ = resume_seq;
    gap_start_time_ = ClockTP{};
    gap_deadline_ = ClockTP{};
    next_delivery_time_ = now;

    // VIDEO_DEBUG_PRINT(
    //     "[JitterBuf] stale prefix drop seq=%u..%u count=%llu age_max=%.2fms expected=%u->%u depth=%zu->%zu",
    //     first_stale_seq, last_stale_seq, static_cast<unsigned long long>(stale_count), max_age_ms, expected_before,
    //     expected_seq_, depth_before, buffered_frames_.size());
}

bool RecvQueueAsync::enqueue(uint8_t *data, size_t size, uint32_t abs_seq)
{
    if (data == nullptr)
        return false;

    const ClockTP arrival = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_)
        return false;

    ++recv_count_;

    if (primed_ && abs_seq < expected_seq_)
    {
        ++late_count_;
        qs_continuity_broken_ = true;
        buffer_ctl_.trigger_pressure(active_tuning_locked());
        update_adaptive_depth_locked();
        return false;
    }

    auto it = buffered_frames_.begin();
    while (it != buffered_frames_.end() && it->seq < abs_seq)
        ++it;
    if (it != buffered_frames_.end() && it->seq == abs_seq)
    {
        ++duplicate_count_;
        return false;
    }

    auto *queued_data = static_cast<uint8_t *>(frame_pool_.allocate(size));
    if (!queued_data)
        return false;

    std::memcpy(queued_data, data, size);
    buffered_frames_.insert(it, {abs_seq, queued_data, size, arrival});
    const auto &tuning = active_tuning_locked();
    arrival_est_.note(abs_seq, arrival, tuning.max_depth);
    reorder_est_.note(abs_seq, tuning.max_depth);
    update_adaptive_depth_locked();

    while (buffered_frames_.size() > active_tuning_locked().max_depth)
    {
        // VIDEO_DEBUG_PRINT("[JitterBuf] buffered frames overflow: size=%zu max=%zu", buffered_frames_.size(),
        //                   active_tuning_locked().max_depth);
        auto tail = std::prev(buffered_frames_.end());
        ++drop_count_;
        ++overflow_count_;
        qs_continuity_broken_ = true;
        drop_frame_locked(tail);
        buffer_ctl_.trigger_pressure(active_tuning_locked());
        update_adaptive_depth_locked();
    }

    cond_.notify_one();
    return true;
}

QueueStatsSnapshot RecvQueueAsync::stats_snapshot_locked() const
{
    const double interval_ms = estimated_interval_ms_locked();
    const double effective_disorder_frames = (std::max)(reorder_est_.depth_frames, reorder_est_.guard_depth_frames);
    const double output_interval_ms = compute_delivery_interval_locked();

    QueueStatsSnapshot out;
    out.fi_short_ms = arrival_est_.interval_short_avg;
    out.fi_avg_ms = interval_ms;
    out.fi_out_ms = output_interval_ms;
    out.jitter_ms = arrival_est_.jitter_avg;
    out.jitter_tail_ms = arrival_est_.jitter_tail;
    out.disorder_fr = effective_disorder_frames;
    out.disorder_max_fr = reorder_est_.max_disorder_depth;
    out.buf_fr = buffered_frames_.size();
    out.depth_target_fr = buffer_ctl_.adaptive_depth;
    out.depth_raw_fr = buffer_ctl_.raw_depth_frames;
    out.pressure_fr = buffer_ctl_.pressure_frames_remaining;
    out.recv = recv_count_;
    out.dlv = deliver_count_;
    out.skip = skip_count_;
    out.drop = drop_count_;
    out.dup = duplicate_count_;
    out.late = late_count_;
    out.reorder = reorder_est_.reorder_cnt;
    out.stale = stale_count_;
    out.ovf = overflow_count_;
    out.allow_immediate = qs_est_.allow_immediate;
    return out;
}

QueueStatsSnapshot RecvQueueAsync::stats_snapshot() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_snapshot_locked();
}

void RecvQueueAsync::handle_gap_locked(std::unique_lock<std::mutex> &lock, ClockTP now)
{
    if (buffered_frames_.empty() || buffered_frames_.front().seq <= expected_seq_)
        return;

    if (gap_start_time_ == ClockTP{})
    {
        gap_start_time_ = now;
        const auto &tuning = active_tuning_locked();
        const double base_interval_ms = (std::max)(tuning.default_frame_interval_ms, estimated_interval_ms_locked());
        gap_deadline_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double, std::milli>(base_interval_ms * tuning.gap_wait_frames));
    }

    const bool depth_pressure = buffered_frames_.size() >= buffer_ctl_.adaptive_depth;
    if (depth_pressure || now >= gap_deadline_)
    {
        skip_gap_locked(buffered_frames_.front().seq);
        return;
    }

    cond_.wait_until(lock, gap_deadline_, [this] {
        return !running_ || buffered_frames_.empty() || buffered_frames_.front().seq <= expected_seq_ ||
               buffered_frames_.size() >= buffer_ctl_.adaptive_depth;
    });
}

void RecvQueueAsync::worker_thread()
{
    set_current_thread_scheduler_policy();

    while (true)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        const auto now = std::chrono::steady_clock::now();
        if (!running_)
        {
            drain_locked(lock);
            break;
        }

        purge_stale_locked(now);

        if (buffered_frames_.empty())
        {
            gap_start_time_ = ClockTP{};
            gap_deadline_ = ClockTP{};
            cond_.wait(lock, [this] { return !running_ || !buffered_frames_.empty(); });
            continue;
        }

        if (!primed_)
        {
            if (buffered_frames_.size() < buffer_ctl_.adaptive_depth)
            {
                cond_.wait(lock, [this] { return !running_ || buffered_frames_.size() >= buffer_ctl_.adaptive_depth; });
                continue;
            }

            primed_ = true;
            expected_seq_ = buffered_frames_.front().seq;
            next_delivery_time_ = now;
            gap_start_time_ = ClockTP{};
            gap_deadline_ = ClockTP{};
            continue;
        }

        if (now < next_delivery_time_)
        {
            cond_.wait_until(lock, next_delivery_time_);
            continue;
        }

        if (buffered_frames_.front().seq < expected_seq_)
        {
            ++drop_count_;
            qs_continuity_broken_ = true;
            drop_frame_locked(buffered_frames_.begin());
            continue;
        }

        if (buffered_frames_.front().seq == expected_seq_)
            deliver_one_locked(lock);
        else
            handle_gap_locked(lock, now);
    }
}

void RecvQueueAsync::deliver_one_locked(std::unique_lock<std::mutex> &lock)
{
    if (buffered_frames_.empty())
        return;

    auto frame = buffered_frames_.front();
    buffered_frames_.pop_front();

    const ClockTP now = std::chrono::steady_clock::now();
    const double expected_interval_ms = estimated_interval_ms_locked();
    const bool allow_immediate = qs_est_.note_delivery(now, expected_interval_ms, qs_continuity_broken_);
    qs_continuity_broken_ = false;

    expected_seq_ = frame.seq + 1;
    gap_start_time_ = ClockTP{};
    gap_deadline_ = ClockTP{};
    buffer_ctl_.consume_pressure();
    update_adaptive_depth_locked();
    next_delivery_time_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double, std::milli>(compute_delivery_interval_locked()));

    ++deliver_count_;
    delivered_frames_.push_back({frame.data, frame.size});

    bool should_release = true;
    lock.unlock();
    try
    {
        if (receive_callback_)
            should_release = receive_callback_(delivered_frames_, allow_immediate);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("[JitterBuf] receive callback threw exception: %s", e.what());
    }
    catch (...)
    {
        VIDEO_ERROR_PRINT("[JitterBuf] receive callback threw unknown exception");
    }
    lock.lock();

    if (should_release)
        clear_delivered_frames_locked();
}

void RecvQueueAsync::skip_gap_locked(uint32_t next_available_seq)
{
    if (!primed_ || next_available_seq <= expected_seq_)
        return;

    const uint32_t missing = next_available_seq - expected_seq_;
    // const double interval_ms = estimated_interval_ms_locked();
    // VIDEO_DEBUG_PRINT(
    //     "[JitterBuf] gap skip expected=%u next=%u miss=%u depth=%zu avg_fi=%.2fms jitter=%.2fms disorder=%.2f",
    //     expected_seq_, next_available_seq, missing, buffered_frames_.size(), interval_ms, arrival_est_.jitter_avg,
    //     reorder_est_.depth_frames);

    skip_count_ += missing;
    qs_continuity_broken_ = true;
    buffer_ctl_.trigger_pressure(active_tuning_locked());
    update_adaptive_depth_locked();

    expected_seq_ = next_available_seq;
    gap_start_time_ = ClockTP{};
    gap_deadline_ = ClockTP{};
    next_delivery_time_ = std::chrono::steady_clock::now();
}

void RecvQueueAsync::drain_locked(std::unique_lock<std::mutex> &lock)
{
    while (!buffered_frames_.empty())
    {
        auto frame = buffered_frames_.front();
        buffered_frames_.pop_front();
        ++deliver_count_;
        delivered_frames_.push_back({frame.data, frame.size});

        bool should_release = true;
        lock.unlock();

        try
        {
            if (receive_callback_)
                should_release = receive_callback_(delivered_frames_, false);
        }
        catch (const std::exception &e)
        {
            VIDEO_ERROR_PRINT("[JitterBuf] receive callback threw exception while draining: %s", e.what());
            should_release = true;
        }
        catch (...)
        {
            VIDEO_ERROR_PRINT("[JitterBuf] receive callback threw unknown exception while draining");
            should_release = true;
        }

        lock.lock();
        if (should_release)
            clear_delivered_frames_locked();
    }

    clear_delivered_frames_locked();
}
