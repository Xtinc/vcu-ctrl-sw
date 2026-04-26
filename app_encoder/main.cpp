#include "RTEncoder.h"
#include "YUVFileIO.h"

extern "C"
{
#include "lib_common/FourCC.h"
#include "lib_rtos/message.h"
}

int main(int argc, char *argv[])
{
    if (argc < 7)
    {
        VIDEO_ERROR_PRINT("Usage: %s file <input.yuv> <output.265> <width> <height> <num_frames> [fourcc=NV12]",
                          argv[0]);
        return EXIT_FAILURE;
    }

    const std::string mode = argv[1];
    if (mode != "file" && mode != "v4l2")
    {
        VIDEO_ERROR_PRINT("Invalid mode: %s (expected: file or v4l2)", mode.c_str());
        return EXIT_FAILURE;
    }

    const std::string inputPath = argv[2];
    const std::string outputPath = argv[3];
    const int width = std::stoi(argv[4]);
    const int height = std::stoi(argv[5]);
    const int numFrames = std::stoi(argv[6]);

    TFourCC fileFourCC = FOURCC(NV12);
    if (argc >= 8)
    {
        fileFourCC = STR2FOURCC(argv[7]);
    }

    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile)
    {
        VIDEO_ERROR_PRINT("Cannot open output file: %s", outputPath.c_str());
        return EXIT_FAILURE;
    }

    EncoderConfig cfg{};
    cfg.eProfile = AL_PROFILE_HEVC_MAIN;
    cfg.uLevel = 51;
    cfg.width = static_cast<uint16_t>(width);
    cfg.height = static_cast<uint16_t>(height);
    cfg.eChromaMode = AL_CHROMA_4_2_2;
    cfg.uBitDepth = 8;
    cfg.eRCMode = AL_RC_CBR;
    cfg.uTargetBitRate = 8000000; /* 8 Mbps */
    cfg.uFrameRate = 30;
    cfg.uClkRatio = 1000;
    cfg.uGopLength = 30;
    cfg.bLowDelayMode = true;
    cfg.uNumB = 0;
    cfg.uNumSrcBufs = 4;
    cfg.uNumStreamBufs = 4;
    cfg.enc_dev_path = "/dev/allegroIP";
    cfg.dma_dev_path = "/dev/dmaproxy";

    int encodedUnits = 0;
    VIDEO_INFO_PRINT("Initializing encoder...");

    RTEncoder<SourceMode::FILE> *encoder = nullptr;
    try
    {
        encoder = new RTEncoder<SourceMode::FILE>(cfg, [&](const uint8_t *pData, size_t size, bool isKeyFrame) {
            VIDEO_INFO_PRINT("[%s] output_unit=%d size=%zu", isKeyFrame ? "Key" : "Normal", encodedUnits, size);
            outFile.write(reinterpret_cast<const char *>(pData), size);
            ++encodedUnits;
        });

        VIDEO_INFO_PRINT("Encoder ready.");
        VIDEO_INFO_PRINT("  Src FourCC : %s", FOURCC2STR(encoder->SRC_FourCC()).c_str());
        VIDEO_INFO_PRINT("  File FourCC: %s", FOURCC2STR(fileFourCC).c_str());

        VIDEO_INFO_PRINT("Processing up to %d frame(s)...", numFrames);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("Encoder initialization failed: %s", e.what());
        return EXIT_FAILURE;
    }

    YuvFileIO yuvFile(inputPath, YuvFileIO::Mode::Read, width, height, fileFourCC);
    if (!yuvFile.open())
    {
        VIDEO_ERROR_PRINT("Failed to open YUV file: %s", inputPath.c_str());
        return EXIT_FAILURE;
    }

    for (int i = 0; i < numFrames; i++)
    {
        if (i > 0 && (i % 90 == 0))
        {
            encoder->request_IDR();
        }

        AL_TBuffer *srcBuf = encoder->acquire_source_buffer();
        if (!srcBuf)
        {
            VIDEO_ERROR_PRINT("Failed to acquire source buffer at frame %d", i);
            break;
        }

        if (!yuvFile.read_frame(srcBuf))
        {
            VIDEO_INFO_PRINT("File EOF at frame %d", i);
            AL_Buffer_Unref(srcBuf);
            break;
        }

        if (!encoder->submit_source_buffer(srcBuf))
        {
            VIDEO_ERROR_PRINT("Failed to submit source buffer at frame %d", i);
            break;
        }
    }

    VIDEO_INFO_PRINT("Flushing encoder...");

    try
    {
        encoder->flush();
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("Error during flush: %s", e.what());
    }

    VIDEO_INFO_PRINT("Output written to: %s, units %d", outputPath.c_str(), encodedUnits);

    delete encoder;
    return EXIT_SUCCESS;
}