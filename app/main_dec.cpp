/**
 * @file main_dec.cpp
 * @brief Decode a compressed bitstream (HEVC or AVC) and display decoded
 *        frames on screen via DRM/KMS zero-copy pipeline using DecMgr.
 *
 * Usage:
 *   vcu_dec <input.hevc|avc> [hevc|avc] [chunk|slice] [--drm <device>] [--fps <rate>]
 *
 *   codec argument defaults to "hevc" when omitted.
 *   --drm device defaults to /dev/dri/card0 when omitted.
 *   --fps rate defaults to 30.0.
 */

#include "DecMgr.h"
#include "SliceFeeder.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr uint32_t kNalChunkSize = 512u * 1024u;       // Max single NAL size for SliceFeeder
static constexpr uint32_t kFrameBufSize = 4u * 1024u * 1024u; // Decoder input buffer for frame mode

enum class FeedMode
{
    Chunk,
    Slice,
};

struct AppOptions
{
    std::string input_path;
    AL_ECodec codec = AL_CODEC_HEVC;
    FeedMode feed_mode = FeedMode::Chunk;
    std::string drm_device = "/dev/dri/card0";
    double fps = 30.0;
};

static const char *codec_name(AL_ECodec codec)
{
    return codec == AL_CODEC_HEVC ? "HEVC" : "AVC";
}

static const char *feed_mode_name(FeedMode feed_mode)
{
    return feed_mode == FeedMode::Slice ? "slice" : "chunk";
}

static bool parse_fps(char const *value, double &fps)
{
    try
    {
        size_t consumed = 0;
        const std::string fps_text(value);
        fps = std::stod(fps_text, &consumed);
        return consumed == fps_text.size() && std::isfinite(fps) && fps > 0.0;
    }
    catch (...)
    {
        return false;
    }
}

static bool parse_options(int argc, char *argv[], AppOptions &options)
{
    if (argc < 2)
    {
        return false;
    }

    options.input_path = argv[1];

    for (int i = 2; i < argc; ++i)
    {
        const std::string token = argv[i];
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

        if (token == "--fps")
        {
            if (i + 1 >= argc || !parse_fps(argv[++i], options.fps))
            {
                VIDEO_ERROR_PRINT("Invalid value for --fps");
                return false;
            }
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

        if (token == "slice")
        {
            options.feed_mode = FeedMode::Slice;
            continue;
        }

        if (token == "chunk")
        {
            options.feed_mode = FeedMode::Chunk;
            continue;
        }

        VIDEO_ERROR_PRINT("Unknown argument '%s'.", token.c_str());
        return false;
    }

    return true;
}

// ---------------------------------------------------------------------------
static void print_usage(const char *app)
{
    VIDEO_ERROR_PRINT("Usage: %s <input.hevc|avc> [hevc|avc] [chunk|slice] [--drm <device>] [--fps <rate>]", app);
    VIDEO_ERROR_PRINT("  Reads the compressed bitstream and displays decoded frames on screen.");
    VIDEO_ERROR_PRINT("  codec argument: hevc (default), h265, avc, or h264");
    VIDEO_ERROR_PRINT("  input mode    : chunk (default) or slice (NAL-based with frame pacing)");
    VIDEO_ERROR_PRINT("  --drm device  : DRM device node (default: /dev/dri/card0)");
    VIDEO_ERROR_PRINT("  --fps rate    : frame delivery rate (default: 30.0, applies to both modes)");
}

static DecMgrConfig make_dec_mgr_config(AppOptions const &options)
{
    DecMgrConfig cfg;
    cfg.dec.codec = options.codec;
    cfg.dec.input_buffer_size = (options.feed_mode == FeedMode::Slice) ? kNalChunkSize : kFrameBufSize;
    cfg.dec.low_delay_mode = (options.feed_mode == FeedMode::Slice);
    cfg.drm.drm_device = options.drm_device;
    return cfg;
}

static std::chrono::steady_clock::duration frame_interval_from_fps(double fps)
{
    return std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(1.0 / fps));
}

static void pace_next_frame(std::chrono::steady_clock::time_point &next_frame_time,
                            std::chrono::steady_clock::duration frame_interval)
{
    std::this_thread::sleep_until(next_frame_time);
    next_frame_time += frame_interval;
}

static bool push_or_stop(DecMgr &dec_mgr, std::vector<uint8_t> const &data, uint8_t flags, char const *mode_name)
{
    if (dec_mgr.push_stream(data.data(), data.size(), flags))
    {
        return true;
    }

    VIDEO_ERROR_PRINT("push_stream failed in %s mode - pipeline has stopped", mode_name);
    return false;
}

static uint32_t feed_slice_mode(std::ifstream &input_file, AppOptions const &options, DecMgr &dec_mgr)
{
    SliceFeeder feeder(options.codec, kNalChunkSize);
    std::vector<uint8_t> nal;
    uint8_t flags = 0;
    uint32_t frames_pushed = 0;
    const auto frame_interval = frame_interval_from_fps(options.fps);
    auto next_frame_time = std::chrono::steady_clock::now();

    VIDEO_INFO_PRINT("Slice mode: pushing NALs with frame pacing at %.2f fps", options.fps);

    while (feeder.feed(input_file, nal, flags))
    {
        if (!push_or_stop(dec_mgr, nal, flags, "slice"))
        {
            break;
        }

        if (flags & AL_STREAM_BUF_FLAG_ENDOFFRAME)
        {
            ++frames_pushed;
            if (frames_pushed % 100 == 0)
                VIDEO_INFO_PRINT("Pushed %u frames (%.2f fps)", frames_pushed, dec_mgr.fps());

            pace_next_frame(next_frame_time, frame_interval);
        }
    }

    if (feeder.failed())
        VIDEO_ERROR_PRINT("Slice-mode input failed");

    return frames_pushed;
}

static uint32_t feed_chunk_mode(std::ifstream &input_file, AppOptions const &options, DecMgr &dec_mgr)
{
    SliceFeeder feeder(options.codec, kNalChunkSize);
    std::vector<uint8_t> frame_data;
    uint32_t frames_pushed = 0;
    const auto frame_interval = frame_interval_from_fps(options.fps);
    auto next_frame_time = std::chrono::steady_clock::now();

    VIDEO_INFO_PRINT("Chunk mode: delivering frames at %.2f fps (interval %.2f ms)", options.fps, 1000.0 / options.fps);

    while (feeder.feed_frame(input_file, frame_data))
    {
        pace_next_frame(next_frame_time, frame_interval);

        if (!push_or_stop(dec_mgr, frame_data, AL_STREAM_BUF_FLAG_UNKNOWN, "chunk"))
        {
            break;
        }

        ++frames_pushed;
        if (frames_pushed % 100 == 0)
            VIDEO_INFO_PRINT("Pushed %u frames (%.2f fps)", frames_pushed, dec_mgr.fps());
    }

    if (feeder.failed())
        VIDEO_ERROR_PRINT("Chunk-mode input failed");

    return frames_pushed;
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

    // ---- Open input -------------------------------------------------------
    std::ifstream input_file(options.input_path, std::ios::binary);
    if (!input_file)
    {
        VIDEO_ERROR_PRINT("Cannot open input bitstream: %s", options.input_path.c_str());
        return EXIT_FAILURE;
    }

    // ---- Build DecMgr config ----------------------------------------------
    DecMgrConfig cfg = make_dec_mgr_config(options);

    // ---- Create DecMgr and start pipeline ---------------------------------
    DecMgr dec_mgr(cfg);
    if (!dec_mgr.start())
    {
        VIDEO_ERROR_PRINT("Failed to start DecMgr pipeline");
        return EXIT_FAILURE;
    }

    VIDEO_INFO_PRINT("DecMgr started on %s (codec: %s, mode: %s, fps: %.2f)", options.drm_device.c_str(),
                     codec_name(options.codec), feed_mode_name(options.feed_mode), options.fps);

    // ---- Feed bitstream with frame rate control ---------------------------
    const uint32_t frames_pushed = (options.feed_mode == FeedMode::Slice)
                                       ? feed_slice_mode(input_file, options, dec_mgr)
                                       : feed_chunk_mode(input_file, options, dec_mgr);

    // ---- Graceful shutdown: DecMgr.stop() handles all cleanup -------------
    VIDEO_INFO_PRINT("Stopping pipeline...");
    dec_mgr.stop();

    VIDEO_INFO_PRINT("Done. Total frames pushed: %u, decode fps: %.2f", frames_pushed, dec_mgr.fps());
    return EXIT_SUCCESS;
}
