// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "RealtimeEncoder.h"

#include <fstream>
#include <memory>
#include <string>

#include "lib_app/UnCompFrameReader.h"
#include "lib_app/YuvIO.h"

#include "lib_conv_yuv/ConvSrc.h"
#include "lib_conv_yuv/AL_RasterConvert.h"

/*****************************************************************************
 * YuvFileSource
 * ---------------------------------------------------------------------------
 * 从裸 YUV 文件读取帧并推送到 RealtimeEncoder 的辅助类。
 *
 * 功能：
 *   - 使用 UnCompFrameReader（lib_app）从文件读取各种 FourCC 格式的 YUV 帧
 *   - 若文件格式与编码器 DMA 缓冲期望的格式不同，自动通过 CYuvSrcConv
 *     （lib_conv_yuv）进行 CPU 格式转换（如 I420 → NV12）
 *   - 使用 RealtimeEncoder::acquireSourceBuffer() / submitSourceBuffer()
 *     直接操作 DMA 缓冲，当不需要格式转换时，无需额外内存拷贝
 *   - 支持循环播放（bLoop=true）
 *
 * 典型用法：
 * @code
 *   EncoderConfig cfg;
 *   cfg.width = 1920; cfg.height = 1080;
 *   RealtimeEncoder enc(cfg, [](const uint8_t* p, size_t n, bool kf){
 *       // 处理码流...
 *   });
 *
 *   AL_TYUVFileInfo fi{};
 *   fi.PictWidth = 1920; fi.PictHeight = 1080;
 *   fi.FourCC    = FOURCC(NV12);  // 文件中存储的格式
 *   fi.FrameRate = 30;
 *
 *   YuvFileSource src(enc, "input.yuv", fi);
 *   while(src.pushNextFrame()) {}  // 推完全部帧，EOF 时返回 false
 *   enc.flush();
 * @endcode
 *****************************************************************************/
class YuvFileSource
{
public:
  /**
   * @brief 构造函数：打开文件并初始化读取器与格式转换器
   *
   * @param enc      目标编码器实例（生命周期须长于本对象）
   * @param filePath YUV 文件路径
   * @param fileInfo 文件格式信息（宽、高、FourCC、帧率）
   * @param bLoop    true = 文件读完后从头循环；false = 读完后停止
   * @throws std::runtime_error 若文件无法打开或资源分配失败
   */
  YuvFileSource(RealtimeEncoder& enc,
                const std::string& filePath,
                const AL_TYUVFileInfo& fileInfo,
                bool bLoop = false);

  ~YuvFileSource();

  /* 禁止拷贝与移动 */
  YuvFileSource(const YuvFileSource&)            = delete;
  YuvFileSource& operator=(const YuvFileSource&) = delete;

  /**
   * @brief 从文件读取下一帧并推送到编码器
   *
   * 内部流程：
   *   1. 调用 acquireSourceBuffer() 获取 DMA 缓冲
   *   2a. 若无需格式转换：直接将文件帧读入 DMA 缓冲
   *   2b. 若需要格式转换：读入 CPU 转换缓冲，再转换到 DMA 缓冲
   *   3. 调用 submitSourceBuffer() 提交给编码器
   *
   * @return true  帧已成功提交
   *         false 文件已读完（bLoop=false 时）或发生错误
   */
  bool pushNextFrame();

  /** 返回已成功推送的帧数 */
  int getFrameCount() const { return m_frameCount; }

private:
  RealtimeEncoder&     m_enc;
  AL_TYUVFileInfo      m_fileInfo;
  std::ifstream        m_file;
  std::unique_ptr<UnCompFrameReader> m_reader;
  std::unique_ptr<IConvSrc>          m_conv;      /*!< 为空时不需要格式转换 */
  AL_TBuffer*                        m_pConvBuf;  /*!< CPU 转换缓冲（格式转换中间buffer） */
  int                                m_frameCount = 0;
};
