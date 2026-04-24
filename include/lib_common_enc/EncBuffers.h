// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/**************************************************************************//*!
   \addtogroup Encoder_Buffers Buffer size
   \ingroup Encoder

   @{
   \file
 *****************************************************************************/
#pragma once

#include "lib_rtos/lib_rtos.h"

#include "lib_common/BufCommon.h"
#include "lib_common/BufferPixMapMeta.h"

#include "lib_common_enc/EncChanParam.h"
#include "lib_common_enc/QPTable.h"

// Encoder Parameter Buf 2 Flag,  Size, Offset
static const AL_TBufInfo EP2_BUF_QP_CTRL =
{
  1, 48, 0
}; // only 20 bytes used
static const AL_TBufInfo EP2_BUF_SEG_CTRL =
{
  2, AL_QPTABLE_SEGMENTS_SIZE, 48
};
static const AL_TBufInfo EP2_BUF_QP_BY_MB =
{
  4, 0, 48 + AL_QPTABLE_SEGMENTS_SIZE
}; // no fixed size

/*************************************************************************//*!
   \brief Retrieves the size of a Encoder parameters buffer 2 (QP Ctrl)
   \param[in] tDim Frame size in pixels
   \param[in] eCodec Codec
   \param[in] uLog2MaxCuSize Max size of a coding unit (log2)
   \return maximum size (in bytes) needed to store
*****************************************************************************/
uint32_t AL_GetAllocSizeEP2(AL_TDimension tDim, AL_ECodec eCodec, uint8_t uLog2MaxCuSize);

// AL_DEPRECATED("Doesn't support pitch different of AL_EncGetMinPitch. Use AL_GetAllocSizeSrc(). Will be removed in 0.9")
uint32_t AL_GetAllocSize_Src(AL_TDimension tDim, uint8_t uBitDepth, AL_EChromaMode eChromaMode, AL_ESrcMode eSrcFmt);

/*************************************************************************//*!
   \brief Retrieves the size of a Source YUV frame buffer
   \param[in] tDim Frame size in pixels
   \param[in] pPicFormat picture format of the frame
   \param[in] iPitch Pitch / stride of the source frame buffer
   \param[in] iStrideHeight The height used for buffer allocation. Might be
   greater than the frame height when frame-height is non 8-multiple, or to
   customize offset between luma and chroma.
   \return maximum size (in bytes) needed for the YUV frame buffer
*****************************************************************************/
uint32_t AL_GetAllocSizeSrc(AL_TDimension tDim, AL_TPicFormat const* pPicFormat, int iPitch, int iStrideHeight);

/*************************************************************************//*!
   \brief Retrieves the size of one pixel component of a YUV frame buffer
   \param[in] pPicFormat picture format of the frame
   \param[in] iPitch Pitch / stride of the source frame buffer
   \param[in] iStrideHeight The height used for buffer allocation
   \param[in] ePlaneId The pixel plane type. Must not be a map plane.
   \return maximum size (in bytes) needed for the component
*****************************************************************************/
uint32_t AL_GetAllocSizeSrc_PixPlane(AL_TPicFormat const* pPicFormat, int iPitch, int iStrideHeight, AL_EPlaneId ePlaneId);

/*************************************************************************//*!
   \brief Retrieves the minimal pitch value supported by the ip depending
   on the source format
   \param[in] iWidth Frame width in pixel unit
   \param[in] pPicFormat Picture format of the frame
   \return pitch value in bytes
*****************************************************************************/
int AL_EncGetMinPitch(int iWidth, AL_TPicFormat const* pPicFormat);

/*************************************************************************//*!
   \brief Retrieves the Source frame buffer storage mode depending on Source mode
   \param[in] eSrcMode Source Mode
   \return Source Storage Mode
*****************************************************************************/
AL_EFbStorageMode AL_GetSrcStorageMode(AL_ESrcMode eSrcMode);

/*************************************************************************//*!
   \brief Check if the Source frame buffer is compressed depending on the Source mode
   \param[in] eSrcMode Source Mode
   \return true if Source is compressed, false otherwise
*****************************************************************************/
bool AL_IsSrcCompressed(AL_ESrcMode eSrcMode);

/*************************************************************************//*!
   \brief Check if the Source frame buffer is interleaved depending on the Source mode
   \param[in] eSrcMode Source Mode
   \return true if source mode is interleaved, false otherwise
*****************************************************************************/
bool AL_IsSrcInterleaved(AL_ESrcMode eSrcMode);

/*************************************************************************//*!
   \brief Check if the Source frame buffer is in MSB format depending on the Source mode
   \param[in] eSrcMode Source Mode
   \return true if source mode is in MSB (Most Significant Bit) format, false otherwise
*****************************************************************************/
bool AL_IsSrcMSB(AL_ESrcMode eSrcMode);

/*@}*/
