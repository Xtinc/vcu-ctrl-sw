// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_rtos/lib_rtos.h"

typedef enum
{
  DEC_FRAME_BUF_NONE = 0,
  DEC_FRAME_BUF_RESERVED = 1,
  DEC_FRAME_BUF_FILLED = 2,
}AL_EDecFrameBufStatus;

typedef struct
{
  bool bFirstSliceValid;
  AL_EDecFrameBufStatus eBufStatus;
  uint16_t uNumSlice;
  bool bIsIntraOnly;
  uint16_t uCurTileID;      // Tile offset of the current tile within the frame
}AL_TDecFrameCtx;

static inline void AL_DecFrameCtx_ResetFlags(AL_TDecFrameCtx* pCtx)
{
  pCtx->bFirstSliceValid = false;
  pCtx->eBufStatus = DEC_FRAME_BUF_NONE;
}

static inline void AL_DecFrameCtx_Reset(AL_TDecFrameCtx* pCtx)
{
  AL_DecFrameCtx_ResetFlags(pCtx);
  pCtx->uNumSlice = 0;
  pCtx->uCurTileID = 0;
  pCtx->bIsIntraOnly = true;
}
