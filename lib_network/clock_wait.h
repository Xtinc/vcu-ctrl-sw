#ifndef CLOCK_WAIT_H
#define CLOCK_WAIT_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

class ClockEntry
{
  public:
    enum class Result
    {
        OK,
        EARLY,
        CANCELLED,
    };

    using ClockTP = std::chrono::steady_clock::time_point;

    ClockEntry();
    ClockEntry(const ClockEntry &) = delete;
    ClockEntry &operator=(const ClockEntry &) = delete;

    Result wait_until(ClockTP target);
    void cancel();
    void reset();

  private:
    std::mutex m_mutex;
    std::condition_variable m_cond;
    std::atomic<bool> m_cancelled;
};

#endif // CLOCK_WAIT_H