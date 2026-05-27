#include "BackGround.h"

extern "C"
{
#include "lib_rtos/message.h"
}
#include <pthread.h>

void set_current_thread_scheduler_policy()
{
    auto thread = pthread_self();
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_RR) - 1;
    pthread_setschedparam(thread, SCHED_RR, &param);
    pthread_setname_np(thread, "video_thd");
}

BackgroundService &BackgroundService::instance()
{
    static BackgroundService instance;
    return instance;
}

BackgroundService::BackgroundService() : work_guard(asio::make_work_guard(io_context))
{
    for (size_t i = 0; i < 2; ++i)
    {
        io_thds.emplace_back([this]() {
            set_current_thread_scheduler_policy();
            io_context.run();
        });
    }

    VIDEO_INFO_PRINT("VideoDriver compiled on %s at %s", __DATE__, __TIME__);
}

BackgroundService::~BackgroundService()
{
    work_guard.reset();
    io_context.stop();

    for (auto &thread : io_thds)
    {
        if (thread.joinable())
        {
            thread.join();
        }
    }
}

asio::io_context &BackgroundService::context()
{
    return io_context;
}
