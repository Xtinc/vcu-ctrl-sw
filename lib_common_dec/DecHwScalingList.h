// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/****************************************************************************
   -----------------------------------------------------------------------------
 **************************************************************************//*!
   \addtogroup lib_common
   @{
   \file
 *****************************************************************************/
#pragma once

#include "lib_common/ScalingList.h"
#include "lib_common/PicFormat.h"

/*************************************************************************//*!
   \brief Dumps Hardware formatted decoder scaling list into buffer of bytes
   \param[in]  pSclLst Pointer to  Scaling list to dump
   \param[in]  eCMode  Chroma subsampling
   \param[out] pBuf    Pointer to buffer that receives the scaling list
                     matrices data
*****************************************************************************/
extern void AL_AVC_WriteDecHwScalingList(AL_TScl const* pSclLst, AL_EChromaMode eCMode, uint8_t* pBuf);

/*************************************************************************//*!
   \brief Dumps Hardware formatted decoder scaling list into buffer of bytes
   \param[in]  pSclLst Pointer to  Scaling list to dump
   \param[out] pBuf    Pointer to buffer that receives the scaling list
                     matrices data
*****************************************************************************/
extern void AL_HEVC_WriteDecHwScalingList(AL_TScl const* pSclLst, uint8_t* pBuf);

/*@}*/
