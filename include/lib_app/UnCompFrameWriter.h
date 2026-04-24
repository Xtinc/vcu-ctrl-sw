// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "BaseFrameWriter.h"

extern "C"
{
#include "lib_common/FourCC.h"
#include "lib_common/BufferAPI.h"
}

/****************************************************************************/
struct UnCompFrameWriter final : IFrameWriter, BaseFrameWriter
{
  UnCompFrameWriter(std::shared_ptr<std::ostream> recFile, AL_EFbStorageMode eStorageMode);

  void WriteFrame(AL_TBuffer* pBuf, AL_TCropInfo* pCrop = nullptr, AL_EPicStruct ePicStruct = AL_PS_FRM);

private:
  void DimInTileCalculusRaster();
};
