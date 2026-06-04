/**
 * @file main_dec_drm.cpp
 * @brief UDP network receive → hardware decode → DRM/KMS display pipeline.
 *
 * Listens on a UDP port for compressed bitstream data (HEVC or AVC) delivered
 * via the ReliableUDP protocol, decodes it with the Allegro VCU hardware, and
 * renders the output on-screen via DRM/KMS zero-copy.
 *
 * Usage:
 *   vcu_dec --port <port> [--drm <device>]
 *
 *   --port <port>   Local UDP port to receive encoded bitstream on (required).
 *   Codec           Fixed to H265/HEVC.
 *   --drm <device>  DRM device node (default: /dev/dri/card0).
 */

#include "DecMgr.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdlib>
#include <pthread.h>
#include <stdexcept>
#include <string>
#include <thread>

static sigset_t g_signal_set;

struct AppOptions
{
    unsigned short udp_port = 0;
    std::string drm_device = "/dev/dri/card0";
    std::string reply_addr;
    unsigned short reply_port = 0;
};

// 4 MB accommodates large I-frames and high-bitrate streams
static constexpr uint32_t kInputBufSize = 4u * 1024u * 1024u;

static void print_usage(const char *app)
{
    VIDEO_ERROR_PRINT("Usage: %s --port <port> [--drm <device>] [--reply-addr <addr> --reply-port <port>]", app);
    VIDEO_ERROR_PRINT("  --port <port>               Local UDP port to receive encoded bitstream on (required).");
    VIDEO_ERROR_PRINT("  codec                       Fixed to H265/HEVC.");
    VIDEO_ERROR_PRINT("  --drm <device>              DRM device node (default: /dev/dri/card0).");
    VIDEO_ERROR_PRINT("  --reply-addr <addr>         Encoder address to reply RTT probes to (optional).");
    VIDEO_ERROR_PRINT("  --reply-port <port>         Encoder UDP port to reply RTT probes to (optional).");
}

static bool parse_options(int argc, char *argv[], AppOptions &options)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string token = argv[i];

        if (token == "--port")
        {
            if (i + 1 >= argc)
            {
                VIDEO_ERROR_PRINT("Missing value for --port");
                return false;
            }
            try
            {
                int port = std::stoi(argv[++i]);
                if (port <= 0 || port > 65535)
                    throw std::out_of_range("port out of range");
                options.udp_port = static_cast<unsigned short>(port);
            }
            catch (...)
            {
                VIDEO_ERROR_PRINT("Invalid port value: %s", argv[i]);
                return false;
            }
            continue;
        }

        if (token == "--drm")
        {
            if (i + 1 >= argc)
            {
                VIDEO_ERROR_PRINT("Missing value for --drm");
                return false;
            }
            options.drm_device = argv[++i];
            continue;
        }

        if (token == "--reply-addr")
        {
            if (i + 1 >= argc)
            {
                VIDEO_ERROR_PRINT("Missing value for --reply-addr");
                return false;
            }
            options.reply_addr = argv[++i];
            continue;
        }

        if (token == "--reply-port")
        {
            if (i + 1 >= argc)
            {
                VIDEO_ERROR_PRINT("Missing value for --reply-port");
                return false;
            }
            try
            {
                int port = std::stoi(argv[++i]);
                if (port <= 0 || port > 65535)
                    throw std::out_of_range("reply-port out of range");
                options.reply_port = static_cast<unsigned short>(port);
            }
            catch (...)
            {
                VIDEO_ERROR_PRINT("Invalid reply-port value: %s", argv[i]);
                return false;
            }
            continue;
        }

        VIDEO_ERROR_PRINT("Unknown argument '%s'.", token.c_str());
        return false;
    }

    if (options.udp_port == 0)
    {
        VIDEO_ERROR_PRINT("--port is required.");
        return false;
    }

    return true;
}

int main(int argc, char *argv[])
{
    message_init();

    AppOptions options;
    if (!parse_options(argc, argv, options))
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    sigemptyset(&g_signal_set);
    sigaddset(&g_signal_set, SIGINT);
    sigaddset(&g_signal_set, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &g_signal_set, nullptr);

    DecMgrConfig cfg;
    cfg.codec = VideoCodec::HEVC;
    cfg.input_buffer_size = kInputBufSize;
    cfg.low_delay_mode = false;
    cfg.drm_device = options.drm_device;
    cfg.udp_local_port = options.udp_port;
    cfg.udp_reply_addr = options.reply_addr;
    cfg.udp_reply_port = options.reply_port;

    DecMgr dec_mgr(cfg);
    if (!dec_mgr.start())
    {
        VIDEO_ERROR_PRINT("Failed to start DecMgr pipeline");
        return EXIT_FAILURE;
    }

    VIDEO_INFO_PRINT("Listening on UDP port %u  codec=HEVC  drm=%s", options.udp_port, options.drm_device.c_str());
    VIDEO_INFO_PRINT("Press Ctrl+C to stop.");

    for (;;)
    {
        timespec timeout;
        timeout.tv_sec = 10;
        timeout.tv_nsec = 0;

        const int signo = sigtimedwait(&g_signal_set, nullptr, &timeout);
        if (signo > 0)
        {
            VIDEO_INFO_PRINT("Signal %d received", signo);
            break;
        }

        if (errno == EAGAIN)
        {
            const double dec_fps = dec_mgr.fps();
            const double recv_bps = dec_mgr.recv_rate();
            const double lost = dec_mgr.lost_rate();
            const std::string queue_stats = dec_mgr.queue_stats_text();
            const int64_t rtt = dec_mgr.rtt_ms();
            const int64_t offset = dec_mgr.offset_ms();
            if (rtt >= 0)
            {
                VIDEO_INFO_PRINT("Dec stats: fps=%.2f, recv_bps=%.0f, lost=%.2f%%, rtt=%" PRId64
                                 "ms, offset=%" PRId64 "ms, %s",
                                 dec_fps, recv_bps, lost * 100.0, rtt, offset, queue_stats.c_str());
            }
            else
            {
                VIDEO_INFO_PRINT("Dec stats: fps=%.2f, recv_bps=%.0f, lost=%.2f%%, rtt=N/A, %s", dec_fps, recv_bps,
                                 lost * 100.0, queue_stats.c_str());
            }
            continue;
        }

        VIDEO_ERROR_PRINT("sigtimedwait failed (errno=%d), stopping decoder", errno);
        break;
    }

    VIDEO_INFO_PRINT("Stopping pipeline (decode fps: %.2f)...", dec_mgr.fps());
    dec_mgr.stop();
    VIDEO_INFO_PRINT("Done.");

    return EXIT_SUCCESS;
}
