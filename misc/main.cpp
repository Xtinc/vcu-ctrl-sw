// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/**
 * @file  example/main.cpp
 * @brief RealtimeEncoder 输入示例（文件源 / v4l2 dmabuf 源）
 *
 * 演示：
 *   1) 文件源：从 YUV 文件读取帧（支持 NV12/I420 等格式）
 *   2) v4l2 源：从 v4l2 capture 导出的 dmabuf fd 直接零拷贝送编码器
 * 经过 VCU 硬件编码为 H.265 码流，并将输出写入文件。
 *
 * 若文件格式（fileFourCC）与编码器 DMA 缓冲格式不同，YuvFileSource 会自动
 * 调用 CYuvSrcConv 进行 CPU 格式转换，无需手动处理。
 *
 * 典型用法（在 ZYNQ 板端运行）：
 *   # 直接输入 NV12 文件（无格式转换）：
 *   ./realtime_encoder_example file input_1920x1080.nv12 output.265 1920 1080 100
 *
 *   # 输入 I420 文件（自动转 NV12）：
 *   ./realtime_encoder_example file input_1920x1080.yuv output.265 1920 1080 100 I420
 *
 *   # v4l2 dmabuf 零拷贝输入（像素格式默认 NV12）：
 *   ./realtime_encoder_example v4l2 /dev/video0 output.265 1920 1080 300 NV12
 */

#include "RealtimeEncoder.h"
#include "YuvFileSource.h"

#if REALTIME_ENCODER_HAS_V4L2
#include "V4l2DmabufSource.h"
#endif
#include <fstream>

extern "C"
{
#include "lib_common/FourCC.h"
#include "lib_rtos/message.h"
}

static uint32_t ParseFourCC(const std::string &fccStr)
{
    if (fccStr.size() != 4)
        throw std::runtime_error("FourCC must be 4 characters");

    return static_cast<uint32_t>((fccStr[0]) | (fccStr[1] << 8) | (fccStr[2] << 16) | (fccStr[3] << 24));
}

int main(int argc, char *argv[])
{
    if (argc < 7)
    {
        VIDEO_ERROR_PRINT("Usage: %s file <input.yuv> <output.265> <width> <height> <num_frames> [fourcc=NV12]",
                          argv[0]);
#if REALTIME_ENCODER_HAS_V4L2
        VIDEO_ERROR_PRINT("   or: %s v4l2 <device> <output.265> <width> <height> <num_frames> [fourcc=NV12]", argv[0]);
#else
        VIDEO_ERROR_PRINT("Note: v4l2 mode is not available in this build (Linux-only).");
#endif
        return EXIT_FAILURE;
    }

    const std::string mode = argv[1];

    if (mode != "file" && mode != "v4l2")
    {
        VIDEO_ERROR_PRINT("Invalid mode: %s (expected: file or v4l2)", mode.c_str());
        return EXIT_FAILURE;
    }

#if !REALTIME_ENCODER_HAS_V4L2
    if (mode == "v4l2")
    {
        VIDEO_ERROR_PRINT("v4l2 mode is not supported in this build (Linux-only).");
        return EXIT_FAILURE;
    }
#endif

    const std::string inputPathOrDevice = argv[2];
    const std::string outputPath = argv[3];
    const int width = std::stoi(argv[4]);
    const int height = std::stoi(argv[5]);
    const int numFrames = std::stoi(argv[6]);

    /* 可选：指定输入 FourCC，默认 NV12 */
    TFourCC fileFourCC = FOURCC(NV12);
    if (argc >= 8)
    {
        const std::string fccStr = argv[7];
        fileFourCC = ParseFourCC(fccStr);
    }

    /* ---- 输出文件 ---- */
    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile)
    {
        VIDEO_ERROR_PRINT("Cannot open output file: %s", outputPath.c_str());
        return EXIT_FAILURE;
    }

    /* ---- 编码器配置 ---- */
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
    cfg.sDevicePath = "/dev/allegroIP";
    cfg.sDMAProxyPath = "/dev/dmaproxy";

    /* ---- 创建编码器 ---- */
    int encodedUnits = 0;
    VIDEO_INFO_PRINT("Initializing encoder...");

    try
    {
        RealtimeEncoder encoder(
            cfg,
            [&](const uint8_t *pData, size_t size, bool isKeyFrame) {
                VIDEO_INFO_PRINT("[%s] output_unit=%d size=%zu", isKeyFrame ? "IDR" : "Normal", encodedUnits, size);
                outFile.write(reinterpret_cast<const char *>(pData), static_cast<std::streamsize>(size));
                ++encodedUnits;
            },
            mode == "v4l2" ? RealtimeEncoder::WorkMode::V4L2 : RealtimeEncoder::WorkMode::FILE);

        VIDEO_INFO_PRINT("Encoder ready.");
        VIDEO_INFO_PRINT("  Src FourCC : %s", FourCC2STR(encoder.getSrcFourCC()).c_str());
        VIDEO_INFO_PRINT("  File FourCC: %s", FourCC2STR(fileFourCC).c_str());
        VIDEO_INFO_PRINT("  PitchY     : %u", encoder.getPitchY());

        VIDEO_INFO_PRINT("Processing up to %d frame(s)...", numFrames);

        int pushed = 0;

        if (mode == "file")
        {
            AL_TYUVFileInfo fileInfo{};
            fileInfo.PictWidth = width;
            fileInfo.PictHeight = height;
            fileInfo.FourCC = fileFourCC;
            fileInfo.FrameRate = cfg.uFrameRate;

            YuvFileSource yuv(encoder, inputPathOrDevice, fileInfo, /*bLoop=*/false);
            for (int i = 0; i < numFrames; ++i)
            {
                if (i > 0 && (i % 90 == 0))
                    encoder.requestKeyFrame();

                if (!yuv.pushNextFrame())
                {
                    VIDEO_INFO_PRINT("  File EOF at frame %d", i);
                    break;
                }
                ++pushed;
            }
        }
        else
        {
#if REALTIME_ENCODER_HAS_V4L2
            V4l2DmabufSource v4l2(encoder, inputPathOrDevice, static_cast<uint32_t>(width),
                                  static_cast<uint32_t>(height), fileFourCC, cfg.uNumSrcBufs);

            for (int i = 0; i < numFrames; ++i)
            {
                if (i > 0 && (i % 90 == 0))
                    encoder.requestKeyFrame();

                if (!v4l2.pushNextFrame())
                {
                    VIDEO_INFO_PRINT("  V4L2 capture stopped at frame %d", i);
                    break;
                }
                ++pushed;
            }
            v4l2.stop();
#else
            VIDEO_ERROR_PRINT("v4l2 mode is not supported in this build (Linux-only).");
            return EXIT_FAILURE;
#endif
        }

        VIDEO_INFO_PRINT("All frames pushed (%d total), flushing encoder...", pushed);
        encoder.flush();

        VIDEO_INFO_PRINT("Done. Pushed frames: %d, output units: %d", pushed, encodedUnits);
        VIDEO_INFO_PRINT("Output written to: %s", outputPath.c_str());
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("Error: %s", e.what());
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
