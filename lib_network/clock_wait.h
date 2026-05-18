#ifndef CLOCK_WAIT_H
#define CLOCK_WAIT_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

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