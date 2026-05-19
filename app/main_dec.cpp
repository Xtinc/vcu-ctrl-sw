/**
 * @file main_dec.cpp
 * @brief Decode a compressed bitstream (HEVC or AVC) and display decoded
 *        frames on screen via DRM/KMS zero-copy pipeline using RTDecoder +
 *        DRMDisplay.
 *
 * Usage:
 *   vcu_dec <input.hevc|avc> [hevc|avc] [chunk|slice] [--drm <device>]
 *
 *   codec argument defaults to "hevc" when omitted.
 *   --drm device defaults to /dev/dri/card0 when omitted.
 */

#include "DRMDisplay.h"
#include "RTDecoder.h"
#include "SliceFeeder.h"

extern "C"
{
#include "lib_common/FourCC.h"
#include "lib_common/PixMapBuffer.h"
#include "lib_rtos/message.h"
}

#include <fstream>
#include <memory>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr uint32_t kChunkSize = 512u * 1024u; // 512 KB matches default input_buffer_size

enum class FeedMode
{
    Chunk,
    Slice,
};

static bool parse_codec_or_mode(std::string const &token, AL_ECodec &codec, FeedMode &feed_mode,
                                std::string &drm_device, int &i, int argc, char *argv[])
{
    if (token == "--drm")
    {
        if (i + 1 < argc)
            drm_device = argv[++i];
        return true;
    }
    if (token == "avc" || token == "h264")
    {
        codec = AL_CODEC_AVC;
        return true;
    }

    if (token == "hevc" || token == "h265")
    {
        codec = AL_CODEC_HEVC;
        return true;
    }

    if (token == "slice")
    {
        feed_mode = FeedMode::Slice;
        return true;
    }

    if (token == "chunk")
    {
        feed_mode = FeedMode::Chunk;
        return true;
    }

    return false;
}

static bool feed_stream_by_chunk(std::ifstream &input_file, RTDecoder &decoder)
{
    std::vector<uint8_t> chunk(kChunkSize);
    while (input_file.good())
    {
        input_file.read(reinterpret_cast<char *>(chunk.data()), static_cast<std::streamsize>(kChunkSize));
        std::streamsize bytes_read = input_file.gcount();
        if (bytes_read <= 0)
        {
            break;
        }

        if (!decoder.push_stream(chunk.data(), static_cast<size_t>(bytes_read)))
        {
            VIDEO_ERROR_PRINT("push_stream failed - decoder has stopped");
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
static void print_usage(const char *app)
{
    VIDEO_ERROR_PRINT("Usage: %s <input.hevc|avc> [hevc|avc] [chunk|slice] [--drm <device>]", app);
    VIDEO_ERROR_PRINT("  Reads the compressed bitstream and displays decoded frames on screen.");
    VIDEO_ERROR_PRINT("  codec argument: hevc (default), h265, avc, or h264");
    VIDEO_ERROR_PRINT("  input mode    : chunk (default) or slice (Annex-B NAL based)");
    VIDEO_ERROR_PRINT("  --drm device  : DRM device node (default: /dev/dri/card0)");
}

// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    message_init();
    if (argc < 2)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const std::string input_path = argv[1];

    // ---- Codec / input mode / DRM device selection -----------------------
    AL_ECodec codec = AL_CODEC_HEVC;
    FeedMode feed_mode = FeedMode::Chunk;
    std::string drm_device = "/dev/dri/card0";

    for (int i = 2; i < argc; ++i)
    {
        const std::string token = argv[i];
        if (!parse_codec_or_mode(token, codec, feed_mode, drm_device, i, argc, argv))
        {
            VIDEO_ERROR_PRINT("Unknown argument '%s'.", token.c_str());
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
    }

    // ---- Open input -------------------------------------------------------
    std::ifstream input_file(input_path, std::ios::binary);
    if (!input_file)
    {
        VIDEO_ERROR_PRINT("Cannot open input bitstream: %s", input_path.c_str());
        return EXIT_FAILURE;
    }

    // ---- Build decoder config ---------------------------------------------
    DecoderConfig cfg;
    cfg.codec = codec;
    cfg.input_buffer_size = kChunkSize;
    cfg.low_delay_mode = (feed_mode == FeedMode::Slice);

    // ---- Create display and decoder ---------------------------------------
    // display is declared before decoder so that, on explicit stop(), the
    // release callback (which calls decoder.return_display_frame) fires while
    // decoder is still alive. Both objects are explicitly drained in order:
    //   1. decoder.flush()  — waits for all frames delivered to callback
    //   2. display->stop()  — drains display thread, releases held frame via callback
    std::unique_ptr<DRMDisplay> display;
    uint32_t frames_shown = 0;

    RTDecoder decoder(cfg, [&](AL_TBuffer *pFrame, AL_TInfoDecode const &info) {
        if (!display)
        {
            // Lazily create DRMDisplay on the first decoded frame so we know
            // a valid frame format is available.
            DRMDisplayConfig drm_cfg;
            drm_cfg.drm_device = drm_device;
            drm_cfg.enable_vsync = true;
            try
            {
                display = std::make_unique<DRMDisplay>(drm_cfg, [&decoder](AL_TBuffer *f) {
                    decoder.return_display_frame(f);
                });
                VIDEO_INFO_PRINT("DRMDisplay opened on %s  (%dx%d)", drm_device.c_str(), info.tDim.iWidth,
                                 info.tDim.iHeight);
            }
            catch (const std::exception &ex)
            {
                VIDEO_ERROR_PRINT("DRMDisplay init failed: %s — dropping frame", ex.what());
                decoder.return_display_frame(pFrame);
                return;
            }
        }

        display->show(pFrame, info);

        if (++frames_shown % 100 == 0)
            VIDEO_INFO_PRINT("Displayed %u frames", frames_shown);
    });

    // ---- Feed bitstream ---------------------------------------------------
    if (feed_mode == FeedMode::Slice)
    {
        SliceFeeder feeder(codec, kChunkSize);
        std::vector<uint8_t> nal;
        uint8_t flags = 0;
        while (feeder.feed(input_file, nal, flags))
        {
            if (!decoder.push_stream(nal.data(), nal.size(), flags))
            {
                VIDEO_ERROR_PRINT("push_stream failed in slice mode - decoder has stopped");
                break;
            }
        }

        if (feeder.failed())
        {
            VIDEO_ERROR_PRINT("Slice-mode input failed");
        }
    }
    else
    {
        if (!feed_stream_by_chunk(input_file, decoder))
        {
            VIDEO_ERROR_PRINT("Chunk-mode input failed");
        }
    }

    // ---- Drain decoder first, then stop display ---------------------------
    // Order matters: flush() waits for all decoded frames to be delivered to
    // the callback (and thus submitted to DRMDisplay). Only after that do we
    // stop DRMDisplay so it can release the last held frame back to the decoder
    // before AL_Decoder_Destroy tears down the buffer pool.
    if (!decoder.flush())
        VIDEO_ERROR_PRINT("Decoder flush timed out; output may be incomplete");

    if (display)
        display->stop();

    VIDEO_INFO_PRINT("Done. Total frames displayed: %u", frames_shown);
    return EXIT_SUCCESS;
}
