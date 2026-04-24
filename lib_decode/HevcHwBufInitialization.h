// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_common/SliceHeader.h"
#include "lib_common_dec/DecPicParam.h"
#include "lib_common/ScalingList.h"

/*************************************************************************//*!
   \brief Initialize buffers required to decode the current frame
   \param[in]  pSclLst Pointer to  Scaling list to dump
   \param[out] pBufs   Pointer to buffers to initialize
*****************************************************************************/
void AL_HEVC_InitHWFrameBuffers(AL_TScl const* pSclLst, AL_TDecPicBuffers* pBufs);

/*************************************************************************//*!
   \brief Initialize buffers required to decode the current slice
   \param[in]  uSliceIndex   Index of the slice to decode
   \param[in]  pSlice        Pointer to the slice header of the current slice
   \param[out] pBufs         Pointer to buffers to initialize
*****************************************************************************/
void AL_HEVC_InitHWSliceBuffers(uint16_t uSliceIndex, AL_THevcSliceHdr const* pSlice, AL_TDecPicBuffers* pBufs);
