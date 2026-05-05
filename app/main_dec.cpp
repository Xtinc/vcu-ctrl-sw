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
#include "YUVFileIO.h"

extern "C"
{
#include "lib_common/FourCC.h"
#include "lib_common/PixMapBuffer.h"
#include "lib_rtos/message.h"
}

#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
static constexpr uint32_t kChunkSize = 512u * 1024u; // 512 KB matches default input_buffer_size

// ---------------------------------------------------------------------------
static void print_usage(const char *app)
{
    VIDEO_ERROR_PRINT("Usage: %s <input.hevc|avc> <output.yuv> [hevc|avc]", app);
    VIDEO_ERROR_PRINT("  Reads the compressed bitstream, decodes it, and writes 10 s of YUV.");
    VIDEO_ERROR_PRINT("  codec argument: hevc (default), h265, avc, or h264");
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

    // ---- Codec selection --------------------------------------------------
    AL_ECodec codec = AL_CODEC_HEVC;
    if (argc >= 4)
    {
        const std::string codec_str = argv[3];
        if (codec_str == "avc" || codec_str == "h264")
        {
            codec = AL_CODEC_AVC;
        }
        else if (codec_str != "hevc" && codec_str != "h265")
        {
            VIDEO_ERROR_PRINT("Unknown codec '%s'. Use hevc or avc.", codec_str.c_str());
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

    bool first_frame = true;
    uint32_t frames_written = 0;

    // ---- Build decoder config ---------------------------------------------
    DecoderConfig cfg;
    cfg.codec = codec;
    cfg.input_buffer_size = kChunkSize;

    // ---- Create decoder and callback --------------------------------------
    RTDecoder decoder(cfg, [&](AL_TBuffer *pFrame, AL_TInfoDecode const &info) {
        std::lock_guard<std::mutex> lock(writer_mutex);

        // Lazy-initialise the YUV writer on the very first decoded frame so
        // we know the exact pixel format, dimensions, and FourCC.
        if (first_frame)
        {
            first_frame = false; // prevent retry even on failure
            TFourCC fourcc = AL_PixMapBuffer_GetFourCC(pFrame);
            int width = info.tDim.iWidth;
            int height = info.tDim.iHeight;

            yuv_writer =
                std::make_unique<YuvFileIO>(output_path, YuvFileIO::Mode::Write, width, height, fourcc);

            if (!yuv_writer->open())
            {
                VIDEO_ERROR_PRINT("Failed to open output file: %s", output_path.c_str());
                yuv_writer.reset();
                return;
            }

            VIDEO_INFO_PRINT("Stream: %dx%d  FourCC=0x%08X  -> %s", width, height,
                             static_cast<unsigned>(fourcc), output_path.c_str());
        }

        if (!yuv_writer || !yuv_writer->is_open())
        {
            return;
        }

        if (!yuv_writer->write_frame(pFrame))
        {
            VIDEO_ERROR_PRINT("Failed to write frame %u", frames_written);
        }
        else
        {
            ++frames_written;
            if (frames_written % 100 == 0)
            {
                VIDEO_INFO_PRINT("Written %u frames", frames_written);
            }
        }
    });

    // ---- Feed bitstream in chunks -----------------------------------------
    std::vector<uint8_t> chunk(kChunkSize);
    while (input_file.good())
    {
        input_file.read(reinterpret_cast<char *>(chunk.data()),
                        static_cast<std::streamsize>(kChunkSize));
        std::streamsize bytes_read = input_file.gcount();
        if (bytes_read <= 0)
        {
            break;
        }

        if (!decoder.push_stream(chunk.data(), static_cast<size_t>(bytes_read)))
        {
            VIDEO_ERROR_PRINT("push_stream failed - decoder has stopped");
            break;
        }
    }

    // ---- Drain and tear down ----------------------------------------------
    if (!decoder.flush())
    {
        VIDEO_ERROR_PRINT("Decoder flush timed out; output may be incomplete");
    }

    if (yuv_writer && yuv_writer->is_open())
    {
        yuv_writer->close();
    }

    VIDEO_INFO_PRINT("Done. Total frames written: %u", frames_written);
    return EXIT_SUCCESS;
}
