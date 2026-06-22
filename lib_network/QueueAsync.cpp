#include "QueueAsync.h"
#include <algorithm>
#include <cmath>
#include <cstring>

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

namespace
{
constexpr auto MIN_FRAME_INTERVAL_MS = 1.0;

// Estimator windows are measured in accepted frame samples.  A small window is
// used for active observations; a large window is used only to forget old tails.
constexpr double FAST_ESTIMATE_WINDOW_FRAMES = 10.0;
constexpr double SLOW_ESTIMATE_WINDOW_FRAMES = 50.0;
constexpr double TAIL_HISTOGRAM_WINDOW_FRAMES = 1000.0;

// Control-shape constants. These are not EMA time scales: they define the
// estimator tail quantile and feed-forward interval sample window.
constexpr double JITTER_TAIL_QUANTILE = 0.95;
constexpr size_t FEEDFORWARD_INTERVAL_WINDOW_FRAMES = 16;
constexpr double JITTER_TAIL_SINGLE_FACTOR = 4.0;

constexpr double DEPTH_SETTLE_EPSILON_FRAMES = 0.25;
constexpr double QS_STABLE_WINDOW_SECONDS = 30.0;
constexpr double QS_MAX_DELIVERY_INTERVAL_FRAMES = 2.0;

constexpr double ema_gain(double window_frames)
{
    return 1.0 / window_frames;
}

constexpr double ema_decay(double window_frames)
{
    return 1.0 - ema_gain(window_frames);
}
} // namespace

template <typename T> static T clamp_value(const T value, const T low, const T high)
{
    return std::max(low, std::min(value, high));
}

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
      jitter_hist(ema_decay(TAIL_HISTOGRAM_WINDOW_FRAMES))
{
}

RJEstimator::~RJEstimator() = default;

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
    if (++window_samples >= FEEDFORWARD_INTERVAL_WINDOW_FRAMES)
    {
        const double elapsed_ms = std::chrono::duration<double, std::milli>(arrival - window_start).count();
        if (elapsed_ms > 0.0)
        {
            const double sample_interval =
                (std::max)(MIN_FRAME_INTERVAL_MS, elapsed_ms / static_cast<double>(window_samples - 1));
            interval_short_avg = sample_interval;
            const double gain = has_interval ? ema_gain(FAST_ESTIMATE_WINDOW_FRAMES) : 1.0;
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
        if (diff > 0.0 && diff < 1000.0 * MIN_FRAME_INTERVAL_MS)
        {
            sample_interval = diff / static_cast<double>(seq_delta);
            sample_valid = sample_interval >= MIN_FRAME_INTERVAL_MS;
        }
    }

    if (sample_valid && has_interval)
    {
        const double late_sample_ms = (std::max)(0.0, sample_interval - interval_avg);
        const double deviation = std::abs(sample_interval - interval_avg);
        jitter_hist.add(late_sample_ms);
        jitter_avg += ema_gain(FAST_ESTIMATE_WINDOW_FRAMES) * (deviation - jitter_avg);

        const double q95_ms = jitter_hist.quantile(JITTER_TAIL_QUANTILE);
        const double tail_sample_ms = std::isfinite(q95_ms) ? (std::max)(q95_ms, late_sample_ms) : late_sample_ms;
        if (tail_sample_ms >= jitter_tail)
            jitter_tail += (std::min)(tail_sample_ms - jitter_tail, JITTER_TAIL_SINGLE_FACTOR * interval_avg);
        else
            jitter_tail += ema_gain(SLOW_ESTIMATE_WINDOW_FRAMES) * (tail_sample_ms - jitter_tail);
    }

    last_seq = abs_seq;
    last_time = arrival;
}

ROEstimator::ROEstimator()
    : reorder_cnt(0), max_disorder_depth(0), depth_frames(0.0), guard_depth_frames(0.0), has_reference(false),
      highest_seq(0)
{
}

ROEstimator::~ROEstimator() = default;

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
        depth_frames *= ema_decay(SLOW_ESTIMATE_WINDOW_FRAMES);
        guard_depth_frames *= ema_decay(SLOW_ESTIMATE_WINDOW_FRAMES);
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
    depth_frames += ema_gain(FAST_ESTIMATE_WINDOW_FRAMES) * (sample - depth_frames);
    guard_depth_frames = (std::max)(guard_depth_frames, sample);
}

void QSEstimator::reset()
{
    allow_immediate = false;
    has_last_delivery_ = false;
    last_delivery_time_ = ClockTP{};
    stable_since_ = ClockTP{};
}

bool QSEstimator::note_delivery(ClockTP now, double expected_interval_ms, bool continuity_broken)
{
    expected_interval_ms = (std::max)(MIN_FRAME_INTERVAL_MS, expected_interval_ms);

    if (continuity_broken)
    {
        reset();
        return allow_immediate;
    }

    bool stable_sample = true;
    if (has_last_delivery_)
    {
        const double actual_interval_ms = std::chrono::duration<double, std::milli>(now - last_delivery_time_).count();
        stable_sample = stable_sample && actual_interval_ms >= 0.0 && actual_interval_ms < 10000.0 &&
                        actual_interval_ms <= QS_MAX_DELIVERY_INTERVAL_FRAMES * expected_interval_ms;
    }

    if (!stable_sample)
    {
        reset();
        return allow_immediate;
    }

    has_last_delivery_ = true;
    last_delivery_time_ = now;

    if (stable_since_ == ClockTP{} || now < stable_since_)
        stable_since_ = now;

    const double stable_seconds = std::chrono::duration<double>(now - stable_since_).count();
    if (stable_seconds >= QS_STABLE_WINDOW_SECONDS)
        allow_immediate = true;

    return allow_immediate;
}

void RecvQueueAsync::BFController::reset(size_t initial_depth)
{
    adaptive_depth = initial_depth;
    decay_depth_frames = static_cast<double>(initial_depth);
    raw_depth_frames = static_cast<double>(initial_depth);
    pressure_frames_remaining = 0;
}

void RecvQueueAsync::BFController::sanitize(const Tunables &tuning)
{
    adaptive_depth =
        clamp_value(adaptive_depth == 0 ? tuning.min_depth : adaptive_depth, tuning.min_depth, tuning.max_depth);
    if (decay_depth_frames <= 0.0)
        decay_depth_frames = static_cast<double>(adaptive_depth);
    decay_depth_frames =
        clamp_value(decay_depth_frames, static_cast<double>(tuning.min_depth), static_cast<double>(tuning.max_depth));
    raw_depth_frames = (std::max)(raw_depth_frames, static_cast<double>(tuning.min_depth));
}

void RecvQueueAsync::BFController::trigger_pressure(const Tunables &tuning)
{
    pressure_frames_remaining = (std::max)(pressure_frames_remaining, tuning.pressure_bonus_frames);
}

void RecvQueueAsync::BFController::consume_pressure()
{
    if (pressure_frames_remaining > 0)
        --pressure_frames_remaining;
}

void RecvQueueAsync::BFController::note(const Tunables &tuning, double reorder_frames, double jitter_frames)
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
        ema_gain(SLOW_ESTIMATE_WINDOW_FRAMES) * (static_cast<double>(target_depth) - decay_depth_frames);
    decay_depth_frames =
        clamp_value(decay_depth_frames, static_cast<double>(tuning.min_depth), static_cast<double>(tuning.max_depth));
    if (decay_depth_frames - static_cast<double>(target_depth) <= DEPTH_SETTLE_EPSILON_FRAMES)
        decay_depth_frames = static_cast<double>(target_depth);

    adaptive_depth = clamp_value(static_cast<size_t>(std::ceil(decay_depth_frames)), target_depth, tuning.max_depth);
}

RecvQueueAsync::RecvQueueAsync()
    : frame_pool_("recv_queue_frame", 64), running_(false), primed_(false), expected_seq_(0)
{
    sanitize_tuning_locked();
}

RecvQueueAsync::~RecvQueueAsync()
{
    stop();
}

void RecvQueueAsync::sanitize_tuning_locked()
{
    tuning_.min_depth = std::max<size_t>(1, tuning_.min_depth);
    tuning_.max_depth = std::max<size_t>(2, (std::max)(tuning_.max_depth, tuning_.min_depth));
    tuning_.min_depth = (std::min)(tuning_.min_depth, tuning_.max_depth);
    tuning_.initial_depth = clamp_value(tuning_.initial_depth, tuning_.min_depth, tuning_.max_depth);

    tuning_.stale_timeout_ms = (std::max)(tuning_.stale_timeout_ms, MIN_FRAME_INTERVAL_MS);
    tuning_.gap_wait_frames = (std::max)(tuning_.gap_wait_frames, 1.0);
    tuning_.default_frame_interval_ms = (std::max)(tuning_.default_frame_interval_ms, MIN_FRAME_INTERVAL_MS);
    tuning_.depth_feedback_gain = clamp_value(tuning_.depth_feedback_gain, 0.0, 0.5);
    tuning_.min_pacing_factor = clamp_value(tuning_.min_pacing_factor, 0.1, 1.0);
    tuning_.max_pacing_factor = (std::max)(tuning_.max_pacing_factor, 1.0);
    if (tuning_.max_pacing_factor < tuning_.min_pacing_factor)
        tuning_.max_pacing_factor = tuning_.min_pacing_factor;
    tuning_.depth_margin_frames = (std::max)(0.0, tuning_.depth_margin_frames);
    tuning_.jitter_weight = (std::max)(0.0, tuning_.jitter_weight);

    buffer_ctl_.sanitize(tuning_);
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
    buffer_ctl_.reset(tuning_.initial_depth);
    qs_est_.reset();
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

    receive_callback_ = std::move(callback);
    sanitize_tuning_locked();
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
    buffer_ctl_.note(tuning_, (std::max)(reorder_est_.depth_frames, reorder_est_.guard_depth_frames),
                     arrival_est_.jitter_tail / estimated_interval_ms_locked());
}

double RecvQueueAsync::estimated_interval_ms_locked() const
{
    if (arrival_est_.interval_avg >= MIN_FRAME_INTERVAL_MS)
        return arrival_est_.interval_avg;

    if (buffered_frames_.size() >= 2)
    {
        const auto &first = buffered_frames_.front();
        const auto &last = buffered_frames_.back();
        const double span_ms = std::chrono::duration<double, std::milli>(last.arrival - first.arrival).count() /
                               static_cast<double>(buffered_frames_.size() - 1);
        if (span_ms >= MIN_FRAME_INTERVAL_MS)
            return span_ms;
    }

    return tuning_.default_frame_interval_ms;
}

double RecvQueueAsync::compute_delivery_interval_locked() const
{
    const double base_interval_ms = estimated_interval_ms_locked();
    const double depth_error =
        static_cast<double>(buffered_frames_.size()) - static_cast<double>(buffer_ctl_.adaptive_depth);
    const double feedback_scale = 1.0 + tuning_.depth_feedback_gain * depth_error;
    const double pacing_factor =
        feedback_scale <= 0.0 ? tuning_.max_pacing_factor
                              : clamp_value(1.0 / feedback_scale, tuning_.min_pacing_factor, tuning_.max_pacing_factor);
    return (std::max)(MIN_FRAME_INTERVAL_MS, base_interval_ms * pacing_factor);
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
                                  std::chrono::duration<double, std::milli>(tuning_.stale_timeout_ms));
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
        buffer_ctl_.trigger_pressure(tuning_);
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
    arrival_est_.note(abs_seq, arrival, tuning_.max_depth);
    reorder_est_.note(abs_seq, tuning_.max_depth);
    update_adaptive_depth_locked();

    while (buffered_frames_.size() > tuning_.max_depth)
    {
        // VIDEO_DEBUG_PRINT("[JitterBuf] buffered frames overflow: size=%zu max=%zu", buffered_frames_.size(),
        //                   tuning_.max_depth);
        auto tail = std::prev(buffered_frames_.end());
        ++drop_count_;
        ++overflow_count_;
        qs_continuity_broken_ = true;
        drop_frame_locked(tail);
        buffer_ctl_.trigger_pressure(tuning_);
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
        const double base_interval_ms = (std::max)(tuning_.default_frame_interval_ms, estimated_interval_ms_locked());
        gap_deadline_ =
            now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::duration<double, std::milli>(base_interval_ms * tuning_.gap_wait_frames));
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
    buffer_ctl_.trigger_pressure(tuning_);
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
