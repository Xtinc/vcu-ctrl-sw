// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "YuvFileSource.h"

#include <chrono>
#include <stdexcept>
#include <string>

extern "C"
{
#include "lib_common/FourCC.h"
#include "lib_common/PixMapBuffer.h"
#include "lib_rtos/message.h"
}

/* ============================================================================
 * 构造 / 析构
 * ============================================================================*/

YuvFileSource::YuvFileSource(RealtimeEncoder &enc, const std::string &filePath, const AL_TYUVFileInfo &fileInfo,
                             bool bLoop)
    : m_enc(enc), m_fileInfo(fileInfo), m_fileIO(nullptr)
{
    TFourCC const dstFourCC = enc.getSrcFourCC();
    if (m_fileInfo.FourCC != dstFourCC)
        throw std::runtime_error("YuvFileSource: unsupported input FourCC (must match encoder source FourCC)");

    /* 1. 创建并打开 YuvFileIO */
    m_fileIO = std::make_unique<YuvFileIO>(filePath, YuvFileIO::Mode::Read, m_fileInfo.PictWidth, m_fileInfo.PictHeight,
                                           m_fileInfo.FourCC, bLoop);
    if (!m_fileIO->open())
        throw std::runtime_error("YuvFileSource: cannot open/read file with YuvFileIO: " + filePath);
}

YuvFileSource::~YuvFileSource() = default;

/* ============================================================================
 * pushNextFrame
 * ============================================================================*/
bool YuvFileSource::pushNextFrame()
{
    /* 从编码器 DMA 缓冲池申请空闲缓冲（阻塞等待） */
    std::chrono::steady_clock::time_point lastPushTime = std::chrono::steady_clock::now();
    AL_TBuffer *pDmaBuf = m_enc.acquireSourceBuffer();
    if (!pDmaBuf)
        return false; /* 编码器已停止或出错 */

    /* 确保 DMA 缓冲设置了正确的图像尺寸 */
    AL_TDimension tDim{m_fileInfo.PictWidth, m_fileInfo.PictHeight};
    AL_PixMapBuffer_SetDimension(pDmaBuf, tDim);
    auto now = std::chrono::steady_clock::now();
    auto waitTime = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastPushTime).count();
    VIDEO_INFO_PRINT("Acquired source buffer (waited %lu msec)", waitTime);

    /* 直接将文件帧读入 DMA 缓冲 */
    if (!m_fileIO->read_frame(pDmaBuf))
    {
        /* 文件 EOF（非循环模式） */
        AL_Buffer_Unref(pDmaBuf);
        return false;
    }
    auto now2 = std::chrono::steady_clock::now();
    auto readTime = std::chrono::duration_cast<std::chrono::milliseconds>(now2 - now).count();
    VIDEO_INFO_PRINT("Read frame from file (elapsed: %lu msec)", readTime);
    /* 将 DMA 缓冲提交给编码器（内部负责 Unref 用户引用） */
    bool const submitted = m_enc.submitSourceBuffer(pDmaBuf);
    if (submitted)
        ++m_frameCount;
    auto now3 = std::chrono::steady_clock::now();
    auto submitTime = std::chrono::duration_cast<std::chrono::milliseconds>(now3 - now2).count();
    VIDEO_INFO_PRINT("Submitted frame to encoder (elapsed: %lu msec)", submitTime);
    return submitted;
}
