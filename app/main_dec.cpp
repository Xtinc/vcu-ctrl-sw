/**
 * @file main_dec.cpp
 * @brief Decode a compressed bitstream (HEVC or AVC) and write 10 seconds of
 *        decoded YUV frames to an output file using the RTDecoder pipeline.
 *
 * Usage:
 *   vcu_dec <input.hevc|avc> <output.yuv> [hevc|avc]
 *
 *   codec argument defaults to "hevc" when omitted.
 */

#include "RTDecoder.h"
#include "SliceFeeder.h"
#include "YUVFileIO.h"

extern "C"
{
#include "lib_common/FourCC.h"
#include "lib_common/PixMapBuffer.h"
#include "lib_rtos/message.h"
}

#include <fstream>
#include <memory>
#include <mutex>
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

static bool parse_codec_or_mode(std::string const &token, AL_ECodec &codec, FeedMode &feed_mode)
{
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
    VIDEO_ERROR_PRINT("Usage: %s <input.hevc|avc> <output.yuv> [hevc|avc] [chunk|slice]", app);
    VIDEO_ERROR_PRINT("  Reads the compressed bitstream, decodes it, and writes 10 s of YUV.");
    VIDEO_ERROR_PRINT("  codec argument: hevc (default), h265, avc, or h264");
    VIDEO_ERROR_PRINT("  input mode    : chunk (default) or slice (Annex-B NAL based)");
}

// ---------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    message_init();
    if (argc < 3)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const std::string input_path = argv[1];
    const std::string output_path = argv[2];

    // ---- Codec / input mode selection ------------------------------------
    AL_ECodec codec = AL_CODEC_HEVC;
    FeedMode feed_mode = FeedMode::Chunk;

    if (argc > 5)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    for (int i = 3; i < argc; ++i)
    {
        const std::string token = argv[i];
        if (!parse_codec_or_mode(token, codec, feed_mode))
        {
            VIDEO_ERROR_PRINT("Unknown argument '%s'. Use codec (hevc/avc) and mode (chunk/slice).", token.c_str());
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

    // ---- Shared state accessed from both the main thread and the SDK
    //      callback thread (protected by writer_mutex where needed).
    std::mutex writer_mutex;
    std::unique_ptr<YuvFileIO> yuv_writer;
    uint32_t frames_written = 0;

    // ---- Build decoder config ---------------------------------------------
    DecoderConfig cfg;
    cfg.codec = codec;
    cfg.input_buffer_size = kChunkSize;
    cfg.low_delay_mode = (feed_mode == FeedMode::Slice);

    // ---- Create decoder and callback --------------------------------------
    RTDecoder decoder(cfg, [&](AL_TBuffer *pFrame, AL_TInfoDecode const &info) {
        std::lock_guard<std::mutex> lock(writer_mutex);
        if (!yuv_writer)
        {
            TFourCC fourcc = AL_PixMapBuffer_GetFourCC(pFrame);
            int width = info.tDim.iWidth;
            int height = info.tDim.iHeight;

            yuv_writer = std::make_unique<YuvFileIO>(output_path, YuvFileIO::Mode::Write, width, height, fourcc);
            if (!yuv_writer->open())
            {
                VIDEO_ERROR_PRINT("Failed to open output file: %s", output_path.c_str());
                return;
            }

            VIDEO_INFO_PRINT("Stream: %dx%d  FourCC=0x%08X  -> %s", width, height, static_cast<unsigned>(fourcc),
                             output_path.c_str());
        }

        if (!yuv_writer->write_frame(pFrame))
        {
            VIDEO_ERROR_PRINT("Failed to write frame %u", frames_written);
            return;
        }

        if (++frames_written % 100 == 0)
        {
            VIDEO_INFO_PRINT("Written %u frames", frames_written);
        }
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

    // ---- Drain and tear down ----------------------------------------------
    if (!decoder.flush())
    {
        VIDEO_ERROR_PRINT("Decoder flush timed out; output may be incomplete");
    }

    if (yuv_writer)
    {
        yuv_writer->close();
    }

    VIDEO_INFO_PRINT("Done. Total frames written: %u", frames_written);
    return EXIT_SUCCESS;
}
