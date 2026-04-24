// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <vector>

extern "C"
{
#include "lib_common/PixMapBuffer.h"
}

#include "lib_app/BufPool.h"

struct PixMapBufPool : public BaseBufPool
{
  void SetFormat(AL_TDimension tDim, TFourCC tFourCC);

  void AddChunk(size_t zSize, const std::vector<AL_TPlaneDescription>& vPlDescriptions);

  bool Init(AL_TAllocator* pAllocator, uint32_t uNumBuf, std::string const& sName);

  AL_TBuffer* CreateBuf(AL_TAllocator* pAllocator, PFN_RefCount_CallBack pRefCntCallBack) override;

private:
  struct PlaneChunk
  {
    size_t zSize;
    std::vector<AL_TPlaneDescription> vPlDescriptions;
  };

  std::vector<PlaneChunk> vPlaneChunks;
  AL_TDimension tDim;
  TFourCC tFourCC;

  std::string sName;
};
