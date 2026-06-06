#ifndef USER_QUEUE_ASYNC_H
#define USER_QUEUE_ASYNC_H

#include "MemPoolUDP.h"
#include <condition_variable>
#include <functional>
#include <thread>

using RecvCallBack = std::function<void(const uint8_t *data, size_t size)>;

/** @brief Asynchronous fixed-depth jitter buffer for fully reassembled frames.
 *
 * This queue trades latency for continuity. It keeps a steady frame cushion
 * near its configured target depth, reorders buffered frames by absolute
 * sequence number, and feeds the decoder at the long-term average receive cadence.
 *
 * Behaviour model:
 *  - Startup prefill waits until the configured startup depth is buffered.
 *  - Normal delivery is paced by a long-term receive-interval estimator.
 *  - Occupancy feedback nudges the delivery interval so buffered depth stays
 *    near the configured target depth instead of draining to zero or growing
 *    unbounded.
 *  - When a sequence gap blocks delivery, the queue waits for late arrival up
 *    to a jitter/reorder-aware deadline, then skips the missing span.
 *
 * Queue statistics are exposed through stats_text(); reading them never mutates
 * queue state. Counters reset only when the queue state is reset.
 */
class UsrQueueAsync
{
    using ClockTP = std::chrono::steady_clock::time_point;
    struct Tunables
    {
        size_t target_depth = 10;
        size_t startup_depth = 10;
        size_t max_buffered_frames = 128;
        double stale_timeout_ms = 2000.0;
        double default_frame_interval_ms = 16.0;
        double depth_feedback_gain = 0.08;
        double min_pacing_factor = 0.70;
        double max_pacing_factor = 1.30;
    };

    struct BufferedFrame
    {
        uint32_t seq;
        uint8_t *data;
        size_t size;
        ClockTP arrival;
    };

    struct Counters
    {
        uint64_t recv = 0;
        uint64_t deliver = 0;
        uint64_t skip = 0;
        uint64_t drop = 0;
        uint64_t duplicate = 0;
        uint64_t late = 0;
        uint64_t reorder = 0;
        uint64_t stale = 0;
        uint64_t overflow = 0;
        uint32_t max_disorder_depth = 0;
    };

  public:
    UsrQueueAsync();
    ~UsrQueueAsync();

    void start(RecvCallBack callback);
    void stop();
    void reset();
    bool enqueue(uint8_t *data, size_t size, uint32_t abs_seq);
    std::string stats_text() const;

  private:
    void worker_thread();
    void sanitize_tuning_locked();
    void reset_state_locked();
    void clear_buffered_frames_locked();
    void drain_locked(std::unique_lock<std::mutex> &lock);
    void update_estimators_locked(uint32_t abs_seq, ClockTP arrival);
    void update_depth_estimate_locked(double depth);
    void note_disorder_locked(uint32_t abs_seq);
    void purge_stale_locked(ClockTP now);
    void drop_frame_locked(std::list<BufferedFrame>::iterator it);
    void release_frame_data(uint8_t *data);
    double bootstrap_interval_locked() const;
    double compute_delivery_interval_locked() const;
    double compute_gap_wait_ms_locked() const;
    void deliver_one_locked(std::unique_lock<std::mutex> &lock);
    void skip_gap_locked(uint32_t next_available_seq);

    MemPool<6, 16> frame_pool_;
    RecvCallBack receive_callback_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    bool running_;

    std::list<BufferedFrame> buffered_frames_;
    bool primed_;
    uint32_t expected_seq_;
    ClockTP next_delivery_time_;
    bool gap_active_;
    ClockTP gap_start_time_;
    Tunables tuning_;

    bool has_arrival_ref_;
    uint32_t last_rate_seq_;
    ClockTP last_rate_time_;
    bool has_highest_arrival_seq_;
    uint32_t highest_arrival_seq_;
    double avg_interval_ms_;
    double jitter_ms_;
    double avg_depth_frames_;
    double disorder_depth_frames_;
    double disorder_guard_depth_frames_;
    Counters counters_;
};

void set_current_thread_scheduler_policy();

#endif // USER_QUEUE_ASYNC_H