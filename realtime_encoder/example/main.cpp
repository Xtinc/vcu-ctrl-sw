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

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

extern "C"
{
#include "lib_common/FourCC.h"
}

static uint32_t ParseFourCC(const std::string& fccStr)
{
  if(fccStr.size() != 4)
    throw std::runtime_error("FourCC must be 4 characters");

  return static_cast<uint32_t>(
    (fccStr[0]) | (fccStr[1] << 8) | (fccStr[2] << 16) | (fccStr[3] << 24));
}

int main(int argc, char* argv[])
{
  if(argc < 7)
  {
    std::cerr << "Usage: " << argv[0]
              << " file <input.yuv> <output.265> <width> <height> <num_frames> [fourcc=NV12]\n"
#if REALTIME_ENCODER_HAS_V4L2
              << "   or: " << argv[0]
              << " v4l2 <device> <output.265> <width> <height> <num_frames> [fourcc=NV12]\n";
#else
              << "\n"
              << "Note: v4l2 mode is not available in this build (Linux-only).\n";
#endif
    return EXIT_FAILURE;
  }

  const std::string mode = argv[1];

  if(mode != "file" && mode != "v4l2")
  {
    std::cerr << "Invalid mode: " << mode << " (expected: file or v4l2)\n";
    return EXIT_FAILURE;
  }

#if !REALTIME_ENCODER_HAS_V4L2
  if(mode == "v4l2")
  {
    std::cerr << "v4l2 mode is not supported in this build (Linux-only).\n";
    return EXIT_FAILURE;
  }
#endif

  const std::string inputPathOrDevice = argv[2];
  const std::string outputPath = argv[3];
  const int         width      = std::stoi(argv[4]);
  const int         height     = std::stoi(argv[5]);
  const int         numFrames  = std::stoi(argv[6]);

  /* 可选：指定输入 FourCC，默认 NV12 */
  TFourCC fileFourCC = FOURCC(NV12);
  if(argc >= 8)
  {
    const std::string fccStr = argv[7];
    fileFourCC = ParseFourCC(fccStr);
  }

  /* ---- 输出文件 ---- */
  std::ofstream outFile(outputPath, std::ios::binary);
  if(!outFile)
  {
    std::cerr << "Cannot open output file: " << outputPath << "\n";
    return EXIT_FAILURE;
  }

  /* ---- 编码器配置 ---- */
  EncoderConfig cfg{};
  cfg.eProfile       = AL_PROFILE_HEVC_MAIN;
  cfg.uLevel         = 51;
  cfg.width          = static_cast<uint16_t>(width);
  cfg.height         = static_cast<uint16_t>(height);
  cfg.eChromaMode    = AL_CHROMA_4_2_0;
  cfg.uBitDepth      = 8;
  cfg.eRCMode        = AL_RC_CBR;
  cfg.uTargetBitRate = 4000000; /* 4 Mbps */
  cfg.uFrameRate     = 30;
  cfg.uClkRatio      = 1000;
  cfg.uGopLength     = 30;
  cfg.bLowDelayMode  = false;
  cfg.uNumB          = 0;
  cfg.uNumSrcBufs    = 4;
  cfg.uNumStreamBufs = 4;
  cfg.sDevicePath    = "/dev/allegroIP";

  /* ---- 创建编码器 ---- */
  int encodedUnits = 0;
  std::cout << "Initializing encoder..." << std::endl;

  try
  {
    RealtimeEncoder encoder(
      cfg,
      [&](const uint8_t* pData, size_t size, bool isKeyFrame)
      {
        if(isKeyFrame)
          std::cout << "  [KeyFrame] output_unit=" << encodedUnits
                    << " size=" << size << "\n";
        outFile.write(reinterpret_cast<const char*>(pData),
                      static_cast<std::streamsize>(size));
        ++encodedUnits;
      });

    std::cout << "Encoder ready.\n";
    std::cout << "  Src FourCC : " << encoder.getSrcFourCC() << "\n";
    std::cout << "  File FourCC: " << fileFourCC << "\n";
    std::cout << "  PitchY     : " << encoder.getPitchY() << "\n";

    std::cout << "Processing up to " << numFrames << " frame(s)...\n";

    int pushed = 0;

    if(mode == "file")
    {
      AL_TYUVFileInfo fileInfo{};
      fileInfo.PictWidth  = width;
      fileInfo.PictHeight = height;
      fileInfo.FourCC     = fileFourCC;
      fileInfo.FrameRate  = cfg.uFrameRate;

      YuvFileSource yuv(encoder, inputPathOrDevice, fileInfo, /*bLoop=*/false);

      for(int i = 0; i < numFrames; ++i)
      {
        if(i > 0 && (i % 90 == 0))
          encoder.requestKeyFrame();

        if(!yuv.pushNextFrame())
        {
          std::cout << "  File EOF at frame " << i << "\n";
          break;
        }
        ++pushed;
      }
    }
    else
    {
#if REALTIME_ENCODER_HAS_V4L2
      V4l2DmabufSource v4l2(encoder,
                            inputPathOrDevice,
                            static_cast<uint32_t>(width),
                            static_cast<uint32_t>(height),
                            fileFourCC,
                            cfg.uNumSrcBufs);

      for(int i = 0; i < numFrames; ++i)
      {
        if(i > 0 && (i % 90 == 0))
          encoder.requestKeyFrame();

        if(!v4l2.pushNextFrame())
        {
          std::cout << "  V4L2 capture stopped at frame " << i << "\n";
          break;
        }
        ++pushed;
      }
      v4l2.stop();
    #else
      std::cerr << "v4l2 mode is not supported in this build (Linux-only).\n";
      return EXIT_FAILURE;
    #endif
    }

    std::cout << "All frames pushed (" << pushed << " total), flushing encoder...\n";
    encoder.flush();

    std::cout << "Done. Pushed frames: " << pushed
          << ", output units: " << encodedUnits << "\n";
    std::cout << "Output written to: " << outputPath << "\n";
  }
  catch(const std::exception& e)
  {
    std::cerr << "Error: " << e.what() << "\n";
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}

