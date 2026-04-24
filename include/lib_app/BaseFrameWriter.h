// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <iostream>
#include <memory>

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_common/FourCC.h"
#include "lib_common/RoundUp.h"
#include "lib_common/SliceConsts.h"
}

struct IFrameWriter
{
  virtual ~IFrameWriter() = default;
  virtual void WriteFrame(AL_TBuffer* pBuf, AL_TCropInfo* pCrop = nullptr, AL_EPicStruct ePicStruct = AL_PS_FRM) = 0;
};

struct BaseFrameWriter
{
  ~BaseFrameWriter();

  BaseFrameWriter(std::shared_ptr<std::ostream> recFile, AL_EFbStorageMode eStorageMode);

protected:
  template<typename T>
  void WriteValue(std::shared_ptr<std::ostream> stream, T pVal);
  void WritePix(const uint8_t* pPix, uint32_t iPitchInPix, uint16_t uHeightInTile, uint32_t uPitchFile);
  void WriteBuffer(std::shared_ptr<std::ostream> stream, const uint8_t* pBuf, uint32_t uWriteSize);
  void CheckNotNull(const uint8_t* pBuf);
  void FactorsCalculus();
  void DimInTileCalculus();

  std::shared_ptr<std::ostream> m_recFile;
  AL_EFbStorageMode m_eStorageMode = AL_FB_MAX_ENUM;
  uint32_t m_uPitchYFile;
  uint32_t m_uPitchCFile;
  uint16_t m_uHeightInTileYFile;
  uint16_t m_uHeightInTileCFile;
  AL_TDimension m_tPicDim = {};
  AL_TPicFormat m_tPicFormat = {};
  TFourCC m_tFourCC = FOURCC(NULL);
  int m_iNbBytesPerPix;
  int m_iChromaVertScale;
  int m_iChromaHorzScale;

  static const std::string ErrorMessageWrite;
  static const std::string ErrorMessageBuffer;
  static const std::string ErrorMessagePitch;
};

/****************************************************************************/
template<typename T>
void BaseFrameWriter::WriteValue(std::shared_ptr<std::ostream> stream, T pVal)
{
  if(stream->fail())
    throw printf("Invalid output file");
  stream->write((char*)&pVal, sizeof(T));

  if(stream->fail())
    throw printf("Invalid output file");
}
