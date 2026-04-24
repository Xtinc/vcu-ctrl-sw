// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "YuvFileSource.h"

#include <stdexcept>
#include <string>

extern "C"
{
#include "lib_common/FourCC.h"
#include "lib_common/PixMapBuffer.h"
}

/* ============================================================================
 * 构造 / 析构
 * ============================================================================*/

YuvFileSource::YuvFileSource(RealtimeEncoder& enc,
                             const std::string& filePath,
                             const AL_TYUVFileInfo& fileInfo,
                             bool bLoop)
  : m_enc(enc)
  , m_fileInfo(fileInfo)
  , m_pConvBuf(nullptr)
{
  /* 1. 打开文件 */
  m_file.open(filePath, std::ios::binary);
  if(!m_file.is_open())
    throw std::runtime_error("YuvFileSource: cannot open file: " + filePath);

  /* 2. 创建 UnCompFrameReader（持有 m_fileInfo 引用，务必在 m_fileInfo 构造后创建） */
  m_reader = std::make_unique<UnCompFrameReader>(m_file, m_fileInfo, bLoop);

  /* 3. 判断是否需要格式转换
   *    编码器 DMA 缓冲的 FourCC 由 enc.getSrcFourCC() 给出；
   *    若与文件 FourCC 不同，则需要 CYuvSrcConv。 */
  TFourCC dstFourCC = enc.getSrcFourCC();
  bool bNeedConv    = (m_fileInfo.FourCC != dstFourCC);

  if(bNeedConv)
  {
    /* 分配 CPU 转换缓冲（CPU 可读写，行步长紧凑，供 UnCompFrameReader 写入） */
    AL_TDimension tDim{ fileInfo.PictWidth, fileInfo.PictHeight };
    m_pConvBuf = AllocateDefaultYuvIOBuffer(tDim, m_fileInfo.FourCC);
    if(!m_pConvBuf)
      throw std::runtime_error("YuvFileSource: failed to allocate conversion buffer");

    /* 构建 CYuvSrcConv 转换器
     *   TFrameInfo 描述的是"输出帧"（即 DMA 缓冲）的格式参数，
     *   CYuvSrcConv::ConvertSrcBuf(uBitDepth, pSrcIn, pSrcOut) 从 pSrcIn（文件格式）
     *   转换到 pSrcOut（DMA 格式） */
    TFrameInfo tFrameInfo{};
    tFrameInfo.tDimension = tDim;
    tFrameInfo.iBitDepth  = enc.getSrcBitDepth();
    tFrameInfo.eCMode     = enc.getChromaMode();

    m_conv = std::unique_ptr<IConvSrc>(new CYuvSrcConv(tFrameInfo));
  }
}

YuvFileSource::~YuvFileSource()
{
  if(m_pConvBuf)
  {
    AL_Buffer_Destroy(m_pConvBuf);
    m_pConvBuf = nullptr;
  }
}

/* ============================================================================
 * pushNextFrame
 * ============================================================================*/

bool YuvFileSource::pushNextFrame()
{
  /* 从编码器 DMA 缓冲池申请空闲缓冲（阻塞等待） */
  AL_TBuffer* pDmaBuf = m_enc.acquireSourceBuffer();
  if(!pDmaBuf)
    return false;  /* 编码器已停止或出错 */

  /* 确保 DMA 缓冲设置了正确的图像尺寸 */
  AL_TDimension tDim{ m_fileInfo.PictWidth, m_fileInfo.PictHeight };
  AL_PixMapBuffer_SetDimension(pDmaBuf, tDim);

  if(m_conv)
  {
    /* ------------------------------------------------------------------
     * 需要格式转换路径
     * 1. 读文件 → CPU 转换缓冲（文件格式）
     * 2. CYuvSrcConv 转换 → DMA 缓冲（编码器格式）
     * ----------------------------------------------------------------*/
    AL_PixMapBuffer_SetDimension(m_pConvBuf, tDim);

    if(!m_reader->ReadFrame(m_pConvBuf))
    {
      /* 文件 EOF（非循环模式），释放已申请的 DMA 缓冲并返回 false */
      AL_Buffer_Unref(pDmaBuf);
      return false;
    }

    m_conv->ConvertSrcBuf(m_enc.getSrcBitDepth(), m_pConvBuf, pDmaBuf);
  }
  else
  {
    /* ------------------------------------------------------------------
     * 无需格式转换路径
     * 直接将文件帧读入 DMA 缓冲（ReadOneFrameYuv 支持 PixMapBuffer）
     * ----------------------------------------------------------------*/
    if(!m_reader->ReadFrame(pDmaBuf))
    {
      /* 文件 EOF（非循环模式） */
      AL_Buffer_Unref(pDmaBuf);
      return false;
    }
  }

  /* 将 DMA 缓冲提交给编码器（内部负责 Unref 用户引用） */
  bool const submitted = m_enc.submitSourceBuffer(pDmaBuf);
  if(submitted)
    ++m_frameCount;

  return submitted;
}
