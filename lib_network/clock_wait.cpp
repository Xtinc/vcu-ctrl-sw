#include "clock_wait.h"
#include <thread>

#if LINUX_OS_ENVIRONMENT
#include <cerrno>
#include <time.h>
#endif

extern "C"
{
#include "lib_rtos/lib_rtos.h"
}

using namespace std::chrono;
using ns = nanoseconds;

static constexpr int64_t MIN_WAIT_NS = 500LL;
static constexpr int64_t SHORT_NS = 500'000LL;
static constexpr int64_t MEDIUM_NS = 2'000'000LL;
static constexpr int64_t PRECISE_NS = 10'000'000LL; // waits <= 10 ms use clock_nanosleep (HRTIMER)

ClockEntry::ClockEntry() : m_cancelled(false)
{
}

int ClockEntry::wait_until(ClockTP target)
{
    auto diff = duration_cast<ns>(target - steady_clock::now()).count();

    if (diff <= 0)
    {
        return (diff == 0) ? 0 : 1;
    }

    while (true)
    {
        if (m_cancelled.load(std::memory_order_acquire))
        {
            return -1;
        }

        diff = duration_cast<ns>(target - steady_clock::now()).count();

        if (diff <= MIN_WAIT_NS)
        {
            return 0;
        }

        if (diff <= SHORT_NS)
        {
#if LINUX_OS_ENVIRONMENT
            int64_t target_ns = duration_cast<ns>(target.time_since_epoch()).count();
            struct timespec end;
            end.tv_sec = target_ns / 1'000'000'000LL;
            end.tv_nsec = target_ns % 1'000'000'000LL;
            while (true)
            {
                int ret = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &end, nullptr);
                if (ret == 0)
                {
                    return 0;
                }
                if (ret == EINTR)
                {
                    if (m_cancelled.load(std::memory_order_acquire))
                    {
                        return -1;
                    }
                    continue;
                }
                break;
            }
#else
            std::this_thread::sleep_until(target);
            if (m_cancelled.load(std::memory_order_acquire))
            {
                return -1;
            }
#endif
            return 0;
        }

        // For waits > SHORT_NS, choose between the precise clock_nanosleep path
        // (≤ PRECISE_NS = 10 ms) and the coarse cond.wait_until path (> 10 ms).
        // clock_nanosleep(TIMER_ABSTIME) uses the kernel HRTIMER subsystem and is
        // unaffected by the scheduler tick (HZ), giving sub-millisecond accuracy
        // even on HZ=100 embedded boards.  For waits longer than 10 ms we still
        // use cond.wait_until (cancellable), but we target PRECISE_NS before the
        // deadline so the precise path always handles the final stretch.
#if LINUX_OS_ENVIRONMENT
        if (diff <= PRECISE_NS)
        {
            // Precise path: sleep in steps of at most MEDIUM_NS so cancellation
            // can be polled between steps.
            const int64_t step_ns = std::min(diff - SHORT_NS, MEDIUM_NS);
            const auto wake_tp = steady_clock::now() + ns{step_ns};
            const int64_t wake_ns = duration_cast<ns>(wake_tp.time_since_epoch()).count();
            struct timespec ts;
            ts.tv_sec  = wake_ns / 1'000'000'000LL;
            ts.tv_nsec = wake_ns % 1'000'000'000LL;
            int ret = ::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr);
            if (ret != 0 && ret != EINTR)
                return 0;
            continue;
        }
        // diff > 10 ms: coarse sleep to (target - PRECISE_NS) via cond.wait_until.
        {
            auto cv_deadline = target - ns{PRECISE_NS};
            std::unique_lock<std::mutex> lk(m_mutex);
            if (m_cancelled.load(std::memory_order_acquire))
            {
                return -1;
            }
            m_cond.wait_until(lk, cv_deadline);
            bool c = m_cancelled.load(std::memory_order_acquire);
            lk.unlock();
            if (c)
            {
                return -1;
            }
        }
#else
        // Non-Linux fallback: original cond.wait_until behaviour.
        {
            auto deadline = (diff < MEDIUM_NS) ? (target - ns{SHORT_NS}) : target;
            std::unique_lock<std::mutex> lk(m_mutex);
            if (m_cancelled.load(std::memory_order_acquire))
            {
                return -1;
            }
            m_cond.wait_until(lk, deadline);
            bool c = m_cancelled.load(std::memory_order_acquire);
            lk.unlock();
            if (c)
            {
                return -1;
            }
        }
#endif
    }
}

void ClockEntry::cancel()
{
    m_cancelled.store(true, std::memory_order_release);
    m_cond.notify_all();
}

void ClockEntry::reset()
{
    m_cancelled.store(false, std::memory_order_release);
}
