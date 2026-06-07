#ifndef USER_QUEUE_ASYNC_H
#define USER_QUEUE_ASYNC_H

#include "MemPoolUDP.h"
#include <array>
#include <condition_variable>
#include <functional>
#include <thread>

using RecvCallBack = std::function<void(const uint8_t *data, size_t size)>;
using SendCallBack = std::function<void(const uint8_t *data, size_t size)>;
using FillCallback = std::function<void(uint8_t *data, size_t size)>;

constexpr size_t SEND_QUEUE_DEPTH = 128;
constexpr size_t SEND_QUEUE_MAX_PACKET_SIZE = 65535;

class SendQueueAsync
{
    struct Slot
    {
        uint8_t *data = nullptr;
        size_t size = 0;
    };

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

    std::array<Slot, SEND_QUEUE_DEPTH> slots_;
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

class RecvQueueAsync
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
    RecvQueueAsync();
    ~RecvQueueAsync();

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
