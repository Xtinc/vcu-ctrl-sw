#include "QueueAsync.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <iomanip>
#include <sstream>

constexpr static auto MIN_FRAME_INTERVAL_MS = 1.0;

template <typename T> static T clamp_value(const T value, const T low, const T high)
{
    return std::max(low, std::min(value, high));
}

void set_current_thread_scheduler_policy()
{
    auto thread = pthread_self();
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_RR) - 1;
    pthread_setschedparam(thread, SCHED_RR, &param);
    pthread_setname_np(thread, "video_thd");
}

// SendQueueAsync implementation
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

// UsrQueueAsync implementation
void RecvQueueAsync::ArrivalEstimator::reset()
{
    has_ref = false;
    last_seq = 0;
    last_time = ClockTP{};
    interval_ms = 0.0;
    jitter_ms = 0.0;
    tail_late_jitter_ms = 0.0;
    late_jitter_hist.reset();
}

void RecvQueueAsync::ArrivalEstimator::note(uint32_t abs_seq, ClockTP arrival, size_t max_seq_delta)
{
    if (!has_ref)
    {
        has_ref = true;
        last_seq = abs_seq;
        last_time = arrival;
        return;
    }

    const int64_t seq_delta = static_cast<int64_t>(abs_seq) - static_cast<int64_t>(last_seq);
    if (seq_delta <= 0)
        return;

    if (seq_delta <= static_cast<int64_t>(max_seq_delta))
    {
        const double diff_ms = std::chrono::duration<double, std::milli>(arrival - last_time).count();
        if (diff_ms > 0.0 && diff_ms < 1000.0)
        {
            constexpr double ALPHA_RATE = 0.02;
            constexpr double ALPHA_JITTER = 0.10;
            const double sample_interval_ms = diff_ms / static_cast<double>(seq_delta);
            if (interval_ms < MIN_FRAME_INTERVAL_MS)
            {
                interval_ms = sample_interval_ms;
                jitter_ms = 0.0;
                tail_late_jitter_ms = 0.0;
            }
            else
            {
                const double late_sample_ms = std::max(0.0, sample_interval_ms - interval_ms);
                const double deviation = std::abs(sample_interval_ms - interval_ms);
                late_jitter_hist.add(late_sample_ms);
                interval_ms += ALPHA_RATE * (sample_interval_ms - interval_ms);
                jitter_ms += ALPHA_JITTER * (deviation - jitter_ms);

                const double q95_ms = late_jitter_hist.quantile(0.95);
                tail_late_jitter_ms = std::isfinite(q95_ms) ? q95_ms : jitter_ms;
            }
        }
    }

    last_seq = abs_seq;
    last_time = arrival;
}

void RecvQueueAsync::ReorderEstimator::reset()
{
    has_highest_seq = false;
    highest_seq = 0;
    depth_frames = 0.0;
    guard_depth_frames = 0.0;
}

void RecvQueueAsync::ReorderEstimator::note(uint32_t abs_seq, size_t max_seq_delta, Counters &counters)
{
    constexpr double ALPHA_DISORDER = 0.10;
    constexpr double DECAY_DISORDER = 0.02;
    constexpr double DECAY_DISORDER_GUARD = 0.01;

    double sample = 0.0;
    if (!has_highest_seq)
    {
        has_highest_seq = true;
        highest_seq = abs_seq;
        return;
    }

    const int64_t seq_delta = static_cast<int64_t>(abs_seq) - static_cast<int64_t>(highest_seq);
    if (seq_delta < 0 && -seq_delta <= static_cast<int64_t>(max_seq_delta))
    {
        sample = static_cast<double>(-seq_delta);
        ++counters.reorder;
        counters.max_disorder_depth = std::max(counters.max_disorder_depth, static_cast<uint32_t>(sample));
    }
    else if (seq_delta > 0)
    {
        highest_seq = abs_seq;
    }

    if (sample > 0.0)
    {
        depth_frames += ALPHA_DISORDER * (sample - depth_frames);
        guard_depth_frames = std::max(guard_depth_frames, sample);
    }
    else
    {
        depth_frames += DECAY_DISORDER * (0.0 - depth_frames);
        guard_depth_frames += DECAY_DISORDER_GUARD * (0.0 - guard_depth_frames);
    }
}

double RecvQueueAsync::ReorderEstimator::effective_depth_frames() const
{
    return std::max(depth_frames, guard_depth_frames);
}

void RecvQueueAsync::DepthController::reset(size_t initial_depth)
{
    adaptive_depth = initial_depth;
    decay_depth_frames = static_cast<double>(initial_depth);
    raw_depth_frames = static_cast<double>(initial_depth);
    pressure_frames_remaining = 0;
}

void RecvQueueAsync::DepthController::sanitize(const Tunables &tuning)
{
    adaptive_depth =
        clamp_value(adaptive_depth == 0 ? tuning.min_depth : adaptive_depth, tuning.min_depth, tuning.max_depth);
    if (decay_depth_frames <= 0.0)
        decay_depth_frames = static_cast<double>(adaptive_depth);
    decay_depth_frames =
        clamp_value(decay_depth_frames, static_cast<double>(tuning.min_depth), static_cast<double>(tuning.max_depth));
    raw_depth_frames = std::max(raw_depth_frames, static_cast<double>(tuning.min_depth));
}

void RecvQueueAsync::DepthController::trigger_pressure(const Tunables &tuning)
{
    pressure_frames_remaining = std::max(pressure_frames_remaining, tuning.pressure_bonus_frames);
}

void RecvQueueAsync::DepthController::consume_pressure()
{
    if (pressure_frames_remaining > 0)
        --pressure_frames_remaining;
}

void RecvQueueAsync::DepthController::update(const Tunables &tuning, double reorder_frames, double jitter_frames)
{
    constexpr double DEPTH_SETTLE_EPSILON = 0.25;
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

    decay_depth_frames += tuning.fall_alpha * (static_cast<double>(target_depth) - decay_depth_frames);
    decay_depth_frames =
        clamp_value(decay_depth_frames, static_cast<double>(tuning.min_depth), static_cast<double>(tuning.max_depth));
    if (decay_depth_frames - static_cast<double>(target_depth) <= DEPTH_SETTLE_EPSILON)
        decay_depth_frames = static_cast<double>(target_depth);

    adaptive_depth = clamp_value(static_cast<size_t>(std::ceil(decay_depth_frames)), target_depth, tuning.max_depth);
}

RecvQueueAsync::RecvQueueAsync()
    : frame_pool_(64), running_(false), primed_(false), expected_seq_(0), gap_active_(false)
{
    sanitize_tuning_locked();
}

RecvQueueAsync::~RecvQueueAsync()
{
    stop();
}

void RecvQueueAsync::sanitize_tuning_locked()
{
    tuning_.max_buffered_frames = std::max<size_t>(2, tuning_.max_buffered_frames);
    tuning_.min_depth = std::max<size_t>(1, tuning_.min_depth);
    tuning_.max_depth = std::max(tuning_.max_depth, tuning_.min_depth);
    tuning_.max_depth = std::min(tuning_.max_depth, tuning_.max_buffered_frames);
    tuning_.min_depth = std::min(tuning_.min_depth, tuning_.max_depth);
    tuning_.initial_depth = clamp_value(tuning_.initial_depth, tuning_.min_depth, tuning_.max_depth);

    tuning_.stale_timeout_ms = std::max(tuning_.stale_timeout_ms, MIN_FRAME_INTERVAL_MS);
    tuning_.default_frame_interval_ms = std::max(tuning_.default_frame_interval_ms, MIN_FRAME_INTERVAL_MS);
    tuning_.depth_feedback_gain = clamp_value(tuning_.depth_feedback_gain, 0.0, 0.5);
    tuning_.min_pacing_factor = clamp_value(tuning_.min_pacing_factor, 0.1, 1.0);
    tuning_.max_pacing_factor = std::max(tuning_.max_pacing_factor, 1.0);
    if (tuning_.max_pacing_factor < tuning_.min_pacing_factor)
        tuning_.max_pacing_factor = tuning_.min_pacing_factor;
    tuning_.depth_margin_frames = std::max(0.0, tuning_.depth_margin_frames);
    tuning_.jitter_weight = std::max(0.0, tuning_.jitter_weight);
    tuning_.fall_alpha = clamp_value(tuning_.fall_alpha, 0.001, 1.0);

    depth_.sanitize(tuning_);
}

void RecvQueueAsync::reset_state_locked()
{
    clear_buffered_frames_locked();
    clear_delivered_frames_locked();
    primed_ = false;
    expected_seq_ = 0;
    next_delivery_time_ = ClockTP{};
    gap_active_ = false;
    gap_start_time_ = ClockTP{};

    arrival_.reset();
    reorder_.reset();
    depth_.reset(tuning_.initial_depth);
    counters_ = Counters{};
}

void RecvQueueAsync::clear_buffered_frames_locked()
{
    for (auto &frame : buffered_frames_)
    {
        release_frame_data(frame.data);
    }
    buffered_frames_.clear();
}

void RecvQueueAsync::clear_delivered_frames_locked()
{
    for (auto &frame : delivered_frames_)
    {
        release_frame_data(frame.data);
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

double RecvQueueAsync::frame_interval_ms_locked() const
{
    return std::max(MIN_FRAME_INTERVAL_MS, bootstrap_interval_locked());
}

void RecvQueueAsync::update_adaptive_depth_locked()
{
    const double base_interval_ms = frame_interval_ms_locked();
    const double jitter_frames = arrival_.tail_late_jitter_ms / base_interval_ms;
    depth_.update(tuning_, reorder_.effective_depth_frames(), jitter_frames);
}

void RecvQueueAsync::trigger_pressure_locked()
{
    depth_.trigger_pressure(tuning_);
    update_adaptive_depth_locked();
}

void RecvQueueAsync::update_estimators_locked(uint32_t abs_seq, ClockTP arrival)
{
    arrival_.note(abs_seq, arrival, tuning_.max_buffered_frames);
    reorder_.note(abs_seq, tuning_.max_buffered_frames, counters_);
    update_adaptive_depth_locked();
}

double RecvQueueAsync::bootstrap_interval_locked() const
{
    if (arrival_.interval_ms >= MIN_FRAME_INTERVAL_MS)
        return arrival_.interval_ms;

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
    const double base_interval_ms = std::max(MIN_FRAME_INTERVAL_MS, bootstrap_interval_locked());
    const double depth_error =
        static_cast<double>(buffered_frames_.size()) - static_cast<double>(depth_.adaptive_depth);
    const double correction = clamp_value(1.0 - tuning_.depth_feedback_gain * depth_error, tuning_.min_pacing_factor,
                                          tuning_.max_pacing_factor);
    return std::max(MIN_FRAME_INTERVAL_MS, base_interval_ms * correction);
}

double RecvQueueAsync::compute_gap_wait_ms_locked() const
{
    const double base_interval_ms = std::max(tuning_.default_frame_interval_ms, bootstrap_interval_locked());
    const double wait_ms = base_interval_ms * static_cast<double>(depth_.adaptive_depth + 1);
    return clamp_value(wait_ms, base_interval_ms, tuning_.stale_timeout_ms);
}

void RecvQueueAsync::drop_frame_locked(std::list<BufferedFrame>::iterator it)
{
    uint8_t *data = it->data;
    buffered_frames_.erase(it);
    release_frame_data(data);
}

void RecvQueueAsync::release_frame_data(uint8_t *data)
{
    frame_pool_.deallocate(data);
}

void RecvQueueAsync::purge_stale_locked(ClockTP now)
{
    const auto cutoff = now - std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double, std::milli>(tuning_.stale_timeout_ms));
    for (auto it = buffered_frames_.begin(); it != buffered_frames_.end();)
    {
        if (it->arrival >= cutoff)
        {
            ++it;
            continue;
        }

        ++counters_.drop;
        ++counters_.stale;
        auto to_drop = it++;
        drop_frame_locked(to_drop);
    }
}

bool RecvQueueAsync::enqueue(uint8_t *data, size_t size, uint32_t abs_seq)
{
    if (data == nullptr)
        return false;

    const ClockTP arrival = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(mutex_);

    if (!running_)
        return false;

    ++counters_.recv;

    if (primed_ && abs_seq < expected_seq_)
    {
        ++counters_.late;
        trigger_pressure_locked();
        return false;
    }

    purge_stale_locked(arrival);

    auto it = buffered_frames_.begin();
    while (it != buffered_frames_.end() && it->seq < abs_seq)
        ++it;
    if (it != buffered_frames_.end() && it->seq == abs_seq)
    {
        ++counters_.duplicate;
        return false;
    }

    auto *queued_data = static_cast<uint8_t *>(frame_pool_.allocate(size));
    if (!queued_data)
        return false;

    std::memcpy(queued_data, data, size);
    buffered_frames_.insert(it, {abs_seq, queued_data, size, arrival});
    update_estimators_locked(abs_seq, arrival);

    while (buffered_frames_.size() > tuning_.max_buffered_frames)
    {
        VIDEO_DEBUG_PRINT("[JitterBuf] buffered frames overflow: size=%zu max=%zu", buffered_frames_.size(),
                          tuning_.max_buffered_frames);
        auto tail = std::prev(buffered_frames_.end());
        ++counters_.drop;
        ++counters_.overflow;
        drop_frame_locked(tail);
        trigger_pressure_locked();
    }

    cond_.notify_one();
    return true;
}

std::string RecvQueueAsync::stats_text() const
{
    std::lock_guard<std::mutex> lock(mutex_);

    const double interval_ms = frame_interval_ms_locked();
    const double tail_jitter_frames = arrival_.tail_late_jitter_ms / interval_ms;

    std::ostringstream os;
    os << std::fixed << std::setprecision(2) << "q_avg_fi=" << interval_ms << "ms, q_jitter=" << arrival_.jitter_ms
       << "ms/" << (arrival_.jitter_ms / interval_ms) << "f, q_tail_jitter=" << arrival_.tail_late_jitter_ms << "ms/"
       << tail_jitter_frames << "f, q_jitter_q=0.95, q_dis=" << reorder_.depth_frames << "f/"
       << counters_.max_disorder_depth << ", q_depth=" << buffered_frames_.size() << '/' << depth_.adaptive_depth
       << ", q_depth_raw=" << depth_.raw_depth_frames << ", q_recv=" << counters_.recv
       << ", q_dlv=" << counters_.deliver << ", q_skip=" << counters_.skip << ", q_drop=" << counters_.drop
       << ", q_dup=" << counters_.duplicate << ", q_late=" << counters_.late << ", q_reorder=" << counters_.reorder
       << ", q_stale=" << counters_.stale << ", q_ovf=" << counters_.overflow;
    counters_ = {};
    return os.str();
}

RecvQueueAsync::WorkerState RecvQueueAsync::worker_state_locked(ClockTP now) const
{
    if (buffered_frames_.empty())
        return WorkerState::Waiting;

    if (!primed_)
        return WorkerState::Priming;

    if (now < next_delivery_time_)
        return WorkerState::Pacing;

    if (buffered_frames_.front().seq <= expected_seq_)
        return WorkerState::Delivering;

    return WorkerState::Gapping;
}

void RecvQueueAsync::handle_gap_locked(std::unique_lock<std::mutex> &lock, ClockTP now)
{
    if (buffered_frames_.empty() || buffered_frames_.front().seq <= expected_seq_)
        return;

    if (!gap_active_)
    {
        gap_active_ = true;
        gap_start_time_ = now;
    }

    const auto gap_deadline =
        gap_start_time_ + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                              std::chrono::duration<double, std::milli>(compute_gap_wait_ms_locked()));
    const size_t pressure_depth = depth_.adaptive_depth;
    const bool depth_pressure = buffered_frames_.size() > pressure_depth;
    if (depth_pressure || now >= gap_deadline)
    {
        skip_gap_locked(buffered_frames_.front().seq);
        return;
    }

    cond_.wait_until(lock, gap_deadline, [this, pressure_depth] {
        return !running_ || buffered_frames_.empty() || buffered_frames_.front().seq <= expected_seq_ ||
               buffered_frames_.size() > pressure_depth;
    });
}

void RecvQueueAsync::worker_thread()
{
    set_current_thread_scheduler_policy();

    while (true)
    {
        std::unique_lock<std::mutex> lock(mutex_);

        const auto now = std::chrono::steady_clock::now();
        purge_stale_locked(now);

        if (!running_)
        {
            drain_locked(lock);
            break;
        }

        switch (worker_state_locked(now))
        {
        case WorkerState::Waiting:
            gap_active_ = false;
            cond_.wait(lock, [this] { return !running_ || !buffered_frames_.empty(); });
            break;
        case WorkerState::Priming:
            if (buffered_frames_.size() < depth_.adaptive_depth)
            {
                cond_.wait(lock, [this] { return !running_ || buffered_frames_.size() >= depth_.adaptive_depth; });
                break;
            }

            primed_ = true;
            expected_seq_ = buffered_frames_.front().seq;
            next_delivery_time_ = now;
            gap_active_ = false;
            break;
        case WorkerState::Pacing:
            cond_.wait_until(lock, next_delivery_time_);
            break;
        case WorkerState::Delivering:
            if (buffered_frames_.front().seq == expected_seq_)
            {
                deliver_one_locked(lock);
                break;
            }

            ++counters_.drop;
            drop_frame_locked(buffered_frames_.begin());
            break;
        case WorkerState::Gapping:
            handle_gap_locked(lock, now);
            break;
        }
    }
}

void RecvQueueAsync::deliver_one_locked(std::unique_lock<std::mutex> &lock)
{
    if (buffered_frames_.empty())
        return;

    auto frame = buffered_frames_.front();
    buffered_frames_.pop_front();

    const ClockTP now = std::chrono::steady_clock::now();
    expected_seq_ = frame.seq + 1;
    gap_active_ = false;
    depth_.consume_pressure();
    update_adaptive_depth_locked();
    next_delivery_time_ = now + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                    std::chrono::duration<double, std::milli>(compute_delivery_interval_locked()));

    ++counters_.deliver;
    delivered_frames_.push_back({frame.data, frame.size});

    bool should_release = true;
    lock.unlock();
    try
    {
        if (receive_callback_)
            should_release = receive_callback_(delivered_frames_);
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
    const double interval_ms = frame_interval_ms_locked();
    VIDEO_DEBUG_PRINT(
        "[JitterBuf] gap skip expected=%u next=%u miss=%u depth=%zu avg_fi=%.2fms jitter=%.2fms disorder=%.2f",
        expected_seq_, next_available_seq, missing, buffered_frames_.size(), interval_ms, arrival_.jitter_ms,
        reorder_.depth_frames);

    counters_.skip += missing;
    trigger_pressure_locked();

    expected_seq_ = next_available_seq;
    gap_active_ = false;
    next_delivery_time_ = std::chrono::steady_clock::now();
}

void RecvQueueAsync::drain_locked(std::unique_lock<std::mutex> &lock)
{
    while (!buffered_frames_.empty())
    {
        auto frame = buffered_frames_.front();
        buffered_frames_.pop_front();
        delivered_frames_.push_back({frame.data, frame.size});

        bool should_release = true;
        lock.unlock();

        try
        {
            if (receive_callback_)
                should_release = receive_callback_(delivered_frames_);
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
