// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_common/PixMapBuffer.h"

/*************************************************************************//*!
   \brief Get the physical address of a plane of a AL_TBuffer
   \param[in] pBuf Pointer to the AL_TBuffer
   \param[in] ePlaneId Type of plane
   \return Returns the physical address of the specified plane if successful,
   0 otherwise
*****************************************************************************/
AL_PADDR AL_PixMapBuffer_GetPlanePhysicalAddress(AL_TBuffer const* pBuf, AL_EPlaneId ePlaneId);

/*************************************************************************//*!
   \brief Get the index of the memory chunk containing the specified plane
   \param[in] pBuf Pointer to the AL_TBuffer
   \param[in] ePlaneId Type of plane
   \return Returns the index of the memory chunk containing the specified plane
*****************************************************************************/
int AL_PixMapBuffer_GetPlaneChunkIdx(AL_TBuffer const* pBuf, AL_EPlaneId ePlaneId);

/*************************************************************************//*!
   \brief Get the list of planes defined in a frame buffer. It might differ
          from the currently used planes if a PixMapBuffer has been over-allocated
          compared to its current use. For instance, if a buffer is allocated
          to handle semiplanar YUVs, but currently only contains monochrome,
          according to its FourCC. Then, current planes will contain Y only,
          while defined planes will contain both Y and UV.
   \param[in] pBuf Pointer to the AL_TBuffer
   \param[out] planes Filled with the list of plane-ids defined in the frame
                      buffer
   \return Returns the number of planes defined
*****************************************************************************/
int AL_PixMapBuffer_GetDefinedPlanes(AL_TBuffer const* pBuf, AL_EPlaneId planes[AL_PLANE_MAX_ENUM]);

/*************************************************************************//*!
   \brief Get the Offset (in bytes) of the pixel at the specified position
          from the base address of the the specified plane
   \param[in] pBuf Pointer to the AL_TBuffer
   \param[in] ePlaneId Type of plane
   \return Returns the Offset (in bytes) of the pixel at the specified position
          from the base of the the specified plane.
   \note The position shall meet the alignment constraint of the buffer
         storage mode; otherwise the behavior is indetermined.
*****************************************************************************/
uint32_t AL_PixMapBuffer_GetPositionOffset(AL_TBuffer const* pBuf, AL_TPosition tPos, AL_EPlaneId ePlaneId);
