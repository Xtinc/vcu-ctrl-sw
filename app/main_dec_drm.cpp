/**
 * @file main_dec_drm.cpp
 * @brief UDP network receive → hardware decode → DRM/KMS display pipeline.
 *
 * Listens on a UDP port for compressed bitstream data (HEVC or AVC) delivered
 * via the ReliableUDP protocol, decodes it with the Allegro VCU hardware, and
 * renders the output on-screen via DRM/KMS zero-copy.
 *
 * Usage:
 *   vcu_dec --port <port> [hevc|avc] [--drm <device>]
 *
 *   --port <port>   Local UDP port to receive encoded bitstream on (required).
 *   hevc / h265     Select HEVC codec (default).
 *   avc  / h264     Select AVC codec.
 *   --drm <device>  DRM device node (default: /dev/dri/card0).
 */

#include "DecMgr.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <thread>

// ---------------------------------------------------------------------------
// Signal handling
// ---------------------------------------------------------------------------
static volatile sig_atomic_t g_stop = 0;

static void signal_handler(int)
{
    g_stop = 1;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------
struct AppOptions
{
    unsigned short udp_port  = 0;
    AL_ECodec      codec     = AL_CODEC_HEVC;
    std::string    drm_device = "/dev/dri/card0";
};

// 4 MB accommodates large I-frames and high-bitrate streams
static constexpr uint32_t kInputBufSize = 4u * 1024u * 1024u;

static void print_usage(const char *app)
{
    VIDEO_ERROR_PRINT("Usage: %s --port <port> [hevc|avc] [--drm <device>]", app);
    VIDEO_ERROR_PRINT("  --port <port>   Local UDP port to receive encoded bitstream on (required).");
    VIDEO_ERROR_PRINT("  hevc / h265     Select HEVC codec (default).");
    VIDEO_ERROR_PRINT("  avc  / h264     Select AVC codec.");
    VIDEO_ERROR_PRINT("  --drm <device>  DRM device node (default: /dev/dri/card0).");
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

        if (token == "avc" || token == "h264")
        {
            options.codec = AL_CODEC_AVC;
            continue;
        }

        if (token == "hevc" || token == "h265")
        {
            options.codec = AL_CODEC_HEVC;
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

// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    message_init();

    AppOptions options;
    if (!parse_options(argc, argv, options))
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    DecMgrConfig cfg;
    cfg.dec.codec             = options.codec;
    cfg.dec.input_buffer_size = kInputBufSize;
    cfg.dec.low_delay_mode    = false;
    cfg.drm.drm_device        = options.drm_device;
    cfg.udp_local_port        = options.udp_port;

    DecMgr dec_mgr(cfg);
    if (!dec_mgr.start())
    {
        VIDEO_ERROR_PRINT("Failed to start DecMgr pipeline");
        return EXIT_FAILURE;
    }

    VIDEO_INFO_PRINT("Listening on UDP port %u  codec=%s  drm=%s",
                     options.udp_port,
                     options.codec == AL_CODEC_HEVC ? "HEVC" : "AVC",
                     options.drm_device.c_str());
    VIDEO_INFO_PRINT("Press Ctrl+C to stop.");

    while (!g_stop)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    VIDEO_INFO_PRINT("Stopping pipeline (decode fps: %.2f)...", dec_mgr.fps());
    dec_mgr.stop();
    VIDEO_INFO_PRINT("Done.");

    return EXIT_SUCCESS;
}
