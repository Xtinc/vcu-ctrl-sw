// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "lib_app/UnCompFrameReader.h"

UnCompFrameReader::UnCompFrameReader(std::ifstream& iRecFile, AL_TYUVFileInfo& tFileInfo, bool bLoopFrames) :
  FrameReader(iRecFile, bLoopFrames), m_tFileInfo(tFileInfo), m_uRndDim(DEFAULT_RND_DIM)
{
}

bool UnCompFrameReader::ReadFrame(AL_TBuffer* pBuffer)
{
  return ReadOneFrameYuv(m_recFile, pBuffer, m_bLoopFile, m_uRndDim);
}

void UnCompFrameReader::SeekA(uint32_t uFrameIdx)
{
  int iPictSize = GetPictureSize(m_tFileInfo);
  m_recFile.seekg(iPictSize * uFrameIdx, std::ios_base::beg);
}

void UnCompFrameReader::SeekR(int iFrameDlt)
{
  int iPictSize = GetPictureSize(m_tFileInfo);
  m_recFile.seekg(iPictSize * iFrameDlt, std::ios_base::cur);
}
