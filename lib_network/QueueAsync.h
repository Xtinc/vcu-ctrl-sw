#ifndef USER_QUEUE_ASYNC_H
#define USER_QUEUE_ASYNC_H

#include "ClockWait.h"
#include "MemPoolUDP.h"
#include "QueueStats.h"
#include <functional>
#include <thread>

struct QueueFrame
{
    uint8_t *data = nullptr;
    size_t size = 0;
};

using RecvCallBack = std::function<bool(const std::vector<QueueFrame> &frames, bool allow_immediate)>;
using SendCallBack = std::function<void(const uint8_t *data, size_t size)>;
using FillCallback = std::function<void(uint8_t *data, size_t size)>;

constexpr size_t SEND_QUEUE_DEPTH = 128;
constexpr size_t SEND_QUEUE_MAX_PACKET_SIZE = 65535;

class SendQueueAsync
{
  public:
    SendQueueAsync();
    ~SendQueueAsync();

    void start(SendCallBack callback);
    void stop();
    bool enqueue(const uint8_t *data, size_t size);
    bool enqueue_fill(size_t size, FillCallback callback);

  private:
    void worker_thread();
    void clear_queued_locked();
    void reset_storage_locked();

    std::array<QueueFrame, SEND_QUEUE_DEPTH> slots_;
    std::array<size_t, SEND_QUEUE_DEPTH> free_indices_;
    std::array<size_t, SEND_QUEUE_DEPTH> queued_indices_;

    size_t free_head_;
    size_t free_tail_;
    size_t free_count_;
    size_t queue_head_;
    size_t queue_tail_;
    size_t queue_count_;
    SendCallBack send_callback_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    bool running_;
};

class RJEstimator
{
    using ClockTP = ClockEntry::ClockTP;

  public:
    RJEstimator();
    ~RJEstimator();

    void reset();
    void note(uint32_t abs_seq, ClockEntry::ClockTP arrival, size_t max_seq_delta);

    double interval_avg;
    double interval_short_avg;
    double jitter_avg;
    double jitter_tail;

  private:
    bool has_reference;
    bool has_interval;
    uint32_t last_seq;
    ClockTP last_time;
    size_t window_samples;
    ClockTP window_start;
    Histogram<20> jitter_hist;
};

class ROEstimator
{
  public:
    ROEstimator();
    ~ROEstimator();

    void reset();
    void note(uint32_t abs_seq, size_t max_seq_delta);

    uint32_t reorder_cnt;
    uint32_t max_disorder_depth;
    double depth_frames;
    double guard_depth_frames;

  private:
    bool has_reference;
    uint32_t highest_seq;
};

class QSEstimator
{
  public:
    using ClockTP = std::chrono::steady_clock::time_point;

    struct Events
    {
        uint64_t skip = 0;
        uint64_t drop = 0;
        uint64_t late = 0;
        uint64_t stale = 0;
        uint64_t overflow = 0;
    };

    void reset();
    bool note_delivery(ClockTP now, double expected_interval_ms, const Events &events);

    bool allow_immediate = false;

  private:
    bool has_last_delivery_ = false;
    ClockTP last_delivery_time_{};
    ClockTP stable_since_{};
};

class RecvQueueAsync
{
    using ClockTP = std::chrono::steady_clock::time_point;

    struct Tunables
    {
        size_t min_depth = 1;
        size_t initial_depth = 8;
        size_t max_depth = 512;
        double stale_timeout_ms = 1000.0;
        double gap_wait_frames = 3.0;
        double default_frame_interval_ms = 2.0;
        double depth_feedback_gain = 0.08;
        double min_pacing_factor = 0.70;
        double max_pacing_factor = 1.50;
        double depth_margin_frames = 0.5;
        double jitter_weight = 1.5;
        uint32_t pressure_bonus_frames = 30;
    };

    struct BufferedFrame
    {
        uint32_t seq;
        uint8_t *data;
        size_t size;
        ClockTP arrival;
    };

    struct BFController
    {
        size_t adaptive_depth = 0;
        double decay_depth_frames = 0.0;
        double raw_depth_frames = 0.0;
        uint32_t pressure_frames_remaining = 0;

        void reset(size_t initial_depth);
        void sanitize(const Tunables &tuning);
        void trigger_pressure(const Tunables &tuning);
        void consume_pressure();
        void note(const Tunables &tuning, double reorder_frames, double jitter_frames);
    };

  public:
    RecvQueueAsync();
    ~RecvQueueAsync();

    void start(RecvCallBack callback);
    void stop();
    void reset();
    bool enqueue(uint8_t *data, size_t size, uint32_t abs_seq);
    QueueStatsSnapshot stats_snapshot() const;

  private:
    void worker_thread();
    void handle_gap_locked(std::unique_lock<std::mutex> &lock, ClockTP now);
    void sanitize_tuning_locked();
    void reset_state_locked();
    void clear_buffered_frames_locked();
    void clear_delivered_frames_locked();
    void drain_locked(std::unique_lock<std::mutex> &lock);
    void update_adaptive_depth_locked();
    void purge_stale_locked(ClockTP now);
    void drop_frame_locked(std::list<BufferedFrame>::iterator it);
    double estimated_interval_ms_locked() const;
    double compute_delivery_interval_locked() const;
    void deliver_one_locked(std::unique_lock<std::mutex> &lock);
    void skip_gap_locked(uint32_t next_available_seq);
    void note_qs_event_locked(const QSEstimator::Events &events);
    QueueStatsSnapshot stats_snapshot_locked() const;

    MemPool<6, 16> frame_pool_;
    RecvCallBack receive_callback_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
    bool running_;

    std::list<BufferedFrame> buffered_frames_;
    std::vector<QueueFrame> delivered_frames_;
    bool primed_;
    uint32_t expected_seq_;
    ClockTP next_delivery_time_;
    ClockTP gap_start_time_;
    ClockTP gap_deadline_;
    Tunables tuning_;

    RJEstimator arrival_est_;
    ROEstimator reorder_est_;
    BFController buffer_ctl_;
    QSEstimator qs_est_;
    QSEstimator::Events qs_events_;
    QueueStatsSnapshot stats_;
};

void set_current_thread_scheduler_policy();

#endif // USER_QUEUE_ASYNC_H
