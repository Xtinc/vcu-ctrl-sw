#include "EncMgr.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <csignal>
#include <pthread.h>

namespace
{
bool block_process_signals(sigset_t &signal_set)
{
    sigemptyset(&signal_set);
    sigaddset(&signal_set, SIGINT);
    sigaddset(&signal_set, SIGTERM);

    const int rc = pthread_sigmask(SIG_BLOCK, &signal_set, nullptr);
    if (rc != 0)
    {
        VIDEO_ERROR_PRINT("Failed to block process signals (rc=%d)", rc);
        return false;
    }

    return true;
}

void print_usage(const char *app)
{
    VIDEO_ERROR_PRINT("Usage: %s <video_device>", app);
}

EncoderConfig make_default_config()
{
    EncoderConfig cfg{};
    cfg.rc_mode = AL_RC_CBR;
    cfg.target_bitrate = 8000000;
    cfg.framerate = 60;
    cfg.clk_ratio = 1000;
    cfg.gop_length = 30;
    cfg.num_b = 0;
    cfg.low_delay_mode = true;
    cfg.width = 3840;
    cfg.height = 2160;
    return cfg;
}
} // namespace

int main(int argc, char *argv[])
{
    message_init();
    if (argc != 2)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    sigset_t signal_set;
    if (!block_process_signals(signal_set))
    {
        return EXIT_FAILURE;
    }

    EncMgrConfig mgr_cfg;
    mgr_cfg.enc = make_default_config();
    mgr_cfg.v4l2_dev = argv[1];
    mgr_cfg.sync_dev = mgr_cfg.enc.low_delay_mode ? "/dev/xlnxsync0" : "";
    mgr_cfg.reconnect_delay_ms = 2000;
    mgr_cfg.max_reconnect_tries = -1; // run until interrupted

    try
    {
        EncMgr mgr(mgr_cfg);

        if (!mgr.start())
        {
            VIDEO_ERROR_PRINT("EncMgr start failed");
            return EXIT_FAILURE;
        }

        VIDEO_INFO_PRINT("V4L2 encoding started. Press Ctrl+C to stop.");
        int signo = 0;
        const int rc = sigwait(&signal_set, &signo);
        if (rc != 0)
        {
            VIDEO_ERROR_PRINT("sigwait failed (rc=%d), stopping encoder", rc);
        }
        else
        {
            VIDEO_INFO_PRINT("Signal %d received", signo);
        }

        VIDEO_INFO_PRINT("Signal received, stopping encoder...");
        mgr.stop();
        VIDEO_INFO_PRINT("V4L2 encoding done");
        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("V4L2 Encoder error: %s", e.what());
        return EXIT_FAILURE;
    }
}
