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

        // For diff in (SHORT_NS, MEDIUM_NS): sleep until SHORT_NS before the target
        // so the precise path handles the final stretch. Otherwise sleep to target.
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

        diff = duration_cast<ns>(target - steady_clock::now()).count();
        if (diff <= MIN_WAIT_NS)
        {
            return 0;
        }
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
