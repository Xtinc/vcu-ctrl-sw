#include "RTEncoder.h"
#include "V4L2Source.h"
#include "YUVFileIO.h"

extern "C"
{
#include "lib_common/FourCC.h"
#include "lib_rtos/message.h"
}

#include <fstream>
#include <memory>
#include <sstream>
#include <vector>

int encode_file_mode(const std::string &cmdFilePath, std::ofstream &outFile, EncoderConfig &cfg)
{
    std::ifstream cmdFile(cmdFilePath);
    if (!cmdFile)
    {
        VIDEO_ERROR_PRINT("Cannot open command file: %s", cmdFilePath.c_str());
        return EXIT_FAILURE;
    }

    std::vector<std::vector<std::string>> cmdLines;
    std::string line;
    while (std::getline(cmdFile, line))
    {
        std::istringstream iss(line);
        std::vector<std::string> tokens;
        for (std::string token; iss >> token;)
            tokens.push_back(token);
        if (!tokens.empty())
            cmdLines.push_back(tokens);
    }
    if (cmdLines.empty())
    {
        VIDEO_ERROR_PRINT("No valid command lines in %s", cmdFilePath.c_str());
        return EXIT_FAILURE;
    }

    TFourCC fourcc = FOURCC(NV12);
    if (cmdLines[0].size() >= 5)
    {
        fourcc = STR2FOURCC(cmdLines[0][4]);
    }
    for (size_t i = 1; i < cmdLines.size(); ++i)
    {
        if (cmdLines[i].size() >= 5 && STR2FOURCC(cmdLines[i][4]) != fourcc)
        {
            VIDEO_ERROR_PRINT("FourCC mismatch at line %zu", i + 1);
            return EXIT_FAILURE;
        }
    }

    int totalEncodedUnits = 0;
    std::unique_ptr<RTEncoder<SourceMode::FILE>> encoder;
    try
    {
        int width_1st = std::stoi(cmdLines[0][1]);
        int height_1st = std::stoi(cmdLines[0][2]);
        cfg.width = static_cast<uint16_t>(width_1st);
        cfg.height = static_cast<uint16_t>(height_1st);
        encoder = std::make_unique<RTEncoder<SourceMode::FILE>>(cfg, [&](const uint8_t *pData, size_t size) {
            VIDEO_INFO_PRINT("[%6d] size: %6zu bytes", totalEncodedUnits, size);
            outFile.write(reinterpret_cast<const char *>(pData), size);
            ++totalEncodedUnits;
        });

        for (size_t cmdIdx = 0; cmdIdx < cmdLines.size(); ++cmdIdx)
        {
            const auto &tokens = cmdLines[cmdIdx];
            if (tokens.size() < 4)
            {
                VIDEO_ERROR_PRINT("Invalid command line at %zu", cmdIdx + 1);
                continue;
            }
            const std::string &inputPath = tokens[0];
            int width = std::stoi(tokens[1]);
            int height = std::stoi(tokens[2]);
            int numFrames = std::stoi(tokens[3]);

            YuvFileIO yuvFile(inputPath, YuvFileIO::Mode::Read, width, height, fourcc);
            if (!yuvFile.open())
            {
                VIDEO_ERROR_PRINT("Failed to open YUV file: %s", inputPath.c_str());
                continue;
            }

            if (width_1st != width || height_1st != height)
            {
                encoder->set_resolution(width, height);
                encoder->request_IDR();
                VIDEO_INFO_PRINT("Resolution change at line %zu: %dx%d", cmdIdx + 1, width, height);
            }
            VIDEO_INFO_PRINT("Start encoding %d frames from %s", numFrames, inputPath.c_str());
            for (int i = 0; i < numFrames; i++)
            {
                AL_TBuffer *srcBuf = encoder->acquire_source_buffer();
                if (!srcBuf)
                {
                    VIDEO_ERROR_PRINT("Failed to acquire source buffer at frame %d", i);
                    break;
                }
                if (!yuvFile.read_frame(srcBuf))
                {
                    AL_Buffer_Unref(srcBuf);
                    break;
                }
                if (!encoder->submit_source_buffer(srcBuf))
                {
                    VIDEO_ERROR_PRINT("Failed to submit source buffer at frame %d", i);
                    break;
                }
            }
        }

        try
        {
            encoder->flush();
        }
        catch (...)
        {
        }
        VIDEO_INFO_PRINT("All tasks done. Total encoded units: %d", totalEncodedUnits);
        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("Encoder error: %s", e.what());
        return EXIT_FAILURE;
    }
}

int encode_v4l2_mode(const std::string &v4l2_dev, std::ofstream &outFile, EncoderConfig &cfg)
{
    int totalEncodedUnits = 0;
    std::unique_ptr<RTEncoder<SourceMode::V4L2_MDMA>> encoder;
    try
    {
        encoder = std::make_unique<RTEncoder<SourceMode::V4L2_MDMA>>(cfg, [&](const uint8_t *pData, size_t size) {
            VIDEO_INFO_PRINT("[%6d] size: %6zu bytes", totalEncodedUnits, size);
            outFile.write(reinterpret_cast<const char *>(pData), size);
            ++totalEncodedUnits;
        });

        V4L2Source v4l2src(v4l2_dev, cfg.width, cfg.height, STR2FOURCC("NV12"), cfg.num_src_bufs);
        encoder->set_release_callback([&v4l2src](int idx, void *) {
            VIDEO_INFO_PRINT("Releasing source buffer index %d back to V4L2 device", idx);
            v4l2src.queue_idx(idx);
        });
        auto fds = encoder->acquire_dma_fds(cfg.num_src_bufs);
        if (!v4l2src.import_fds(fds))
        {
            VIDEO_ERROR_PRINT("Failed to import dma fds to V4L2 device");
            return EXIT_FAILURE;
        }

        if (!v4l2src.start())
        {
            VIDEO_ERROR_PRINT("Failed to start V4L2 device: %s", v4l2_dev.c_str());
            return EXIT_FAILURE;
        }

        int frameCount = 1000;
        for (int i = 0; i < frameCount; ++i)
        {
            int idx = v4l2src.dequeue_idx();
            if (idx < 0)
            {
                VIDEO_ERROR_PRINT("Failed to read frame from V4L2 device at frame %d", i);
                break;
            }
            if (!encoder->submit_dma_index(idx))
            {
                VIDEO_ERROR_PRINT("Failed to submit dma fd at frame %d", i);
                break;
            }
        }

        try
        {
            encoder->flush();
        }
        catch (...)
        {
        }
        v4l2src.stop();
        VIDEO_INFO_PRINT("V4L2 encoding done. Total encoded units: %d", totalEncodedUnits);
        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("V4L2 Encoder error: %s", e.what());
        return EXIT_FAILURE;
    }
}

int main(int argc, char *argv[])
{
    if (argc < 4)
    {
        VIDEO_ERROR_PRINT("Usage: %s <mode> <cmd.txt|v4l2_dev> <output.265>", argv[0]);
        VIDEO_ERROR_PRINT("  mode: file | v4l2");
        VIDEO_ERROR_PRINT("  file: %s file <cmd.txt> <output.265>", argv[0]);
        VIDEO_ERROR_PRINT("  v4l2: %s v4l2 <video_device> <output.265>", argv[0]);
        return EXIT_FAILURE;
    }

    const std::string mode = argv[1];
    if (mode != "file" && mode != "v4l2")
    {
        VIDEO_ERROR_PRINT("Invalid mode: %s (expected: file or v4l2)", mode.c_str());
        return EXIT_FAILURE;
    }

    EncoderConfig cfg{};
    cfg.profile = AL_PROFILE_HEVC_MAIN;
    cfg.level = 51;
    cfg.chroma_mode = AL_CHROMA_4_2_0;
    cfg.bit_depth = 8;
    cfg.rc_mode = AL_RC_CBR;
    cfg.target_bitrate = 8000000;
    cfg.framerate = 60;
    cfg.clk_ratio = 1000;
    cfg.gop_length = 30;
    cfg.low_delay_mode = false;
    cfg.num_b = 0;
    cfg.num_src_bufs = 4;
    cfg.num_stream_bufs = 4;
    cfg.enc_dev_path = "/dev/allegroIP";
    cfg.dma_dev_path = "/dev/dmaproxy";

    std::ofstream outFile(argv[3], std::ios::binary);
    if (!outFile)
    {
        VIDEO_ERROR_PRINT("Cannot open output file: %s", argv[3]);
        return EXIT_FAILURE;
    }

    if (mode == "file")
    {
        return encode_file_mode(argv[2], outFile, cfg);
    }
    else if (mode == "v4l2")
    {
        cfg.width = 3840;
        cfg.height = 2160;
        return encode_v4l2_mode(argv[2], outFile, cfg);
    }

    return EXIT_FAILURE;
}