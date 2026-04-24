// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "lib_app/FrameReader.h"
#include "lib_app/YuvIO.h"

class UnCompFrameReader : public FrameReader
{
private:
  AL_TYUVFileInfo& m_tFileInfo;
  uint32_t m_uRndDim;

public:
  UnCompFrameReader(std::ifstream& File, AL_TYUVFileInfo& tFileInfo, bool bLoopFrames);
  virtual bool ReadFrame(AL_TBuffer* pFrameBuffer);

  void SeekA(uint32_t uFrameIdx); // seek to Absolution position from the beginning
  void SeekR(int iFrameDlt);      // seek to Relative position from the current position (both direction allowed)

  void SetRndDim(uint32_t uRndDim) { m_uRndDim = uRndDim; };
};
