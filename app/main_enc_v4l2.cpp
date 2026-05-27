#include "EncMgr.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <csignal>
#include <cerrno>
#include <cstdlib>
#include <pthread.h>
#include <stdexcept>

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
    VIDEO_ERROR_PRINT("Usage: %s <video_device> <v4l2_subdev> <udp_dest_addr> <udp_dest_port> [udp_local_port]", app);
}

EncoderConfig make_default_config()
{
    EncoderConfig cfg{};
    cfg.rc_mode = AL_RC_CBR;
    cfg.profile = AL_PROFILE_HEVC_MAIN;
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

int main(int argc, char *argv[])
{
    message_init();
    if (argc != 5 && argc != 6)
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
    mgr_cfg.v4l2_subdev = argv[2];
    mgr_cfg.udp_dest_addr = argv[3];

    try
    {
        const int udp_dest_port = std::stoi(argv[4]);
        if (udp_dest_port <= 0 || udp_dest_port > 65535)
        {
            throw std::out_of_range("udp_dest_port out of range");
        }
        mgr_cfg.udp_dest_port = static_cast<unsigned short>(udp_dest_port);

        if (argc == 6)
        {
            const int udp_local_port = std::stoi(argv[5]);
            if (udp_local_port < 0 || udp_local_port > 65535)
            {
                throw std::out_of_range("udp_local_port out of range");
            }
            mgr_cfg.udp_local_port = static_cast<unsigned short>(udp_local_port);
        }
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("Invalid UDP argument: %s", e.what());
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    mgr_cfg.sync_dev = mgr_cfg.enc.low_delay_mode ? "/dev/xlnxsync0" : "";
    mgr_cfg.source_check_interval_ms = 2000;

    try
    {
        EncMgr mgr(mgr_cfg);

        if (!mgr.start())
        {
            VIDEO_ERROR_PRINT("EncMgr start failed");
            return EXIT_FAILURE;
        }

        VIDEO_INFO_PRINT("V4L2 encoding started. Press Ctrl+C to stop.");
        for (;;)
        {
            timespec timeout;
            timeout.tv_sec = 10;
            timeout.tv_nsec = 0;

            const int signo = sigtimedwait(&signal_set, nullptr, &timeout);
            if (signo > 0)
            {
                VIDEO_INFO_PRINT("Signal %d received", signo);
                break;
            }

            if (errno == EAGAIN)
            {
                const auto stats = mgr.fps();
                const double send_bps = mgr.send_rate();
                VIDEO_INFO_PRINT("Enc stats: fps=%.2f, bps=%.0f, send_bps=%.0f", stats.first, stats.second,
                                 send_bps);
                continue;
            }

            VIDEO_ERROR_PRINT("sigtimedwait failed (errno=%d), stopping encoder", errno);
            break;
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
