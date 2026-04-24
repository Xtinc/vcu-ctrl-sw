// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/**************************************************************************//*!
   \addtogroup Buffers
   @{
   \file
 *****************************************************************************/

#pragma once

#include "lib_common/SliceConsts.h"
#include "lib_common/PixMapBuffer.h"

/*************************************************************************//*!
   \brief Reconstructed picture information
*****************************************************************************/
typedef struct AL_TReconstructedInfo
{
  uint32_t uID;
  AL_EPicStruct ePicStruct; /*!< Specifies the pic_struct: subset of table D-1 */
  uint32_t iPOC; /*!< the Picture Order Count of the frame buffer */
  AL_TDimension tPicDim; /*!< The dimension of the reconstructed frame buffer */
}AL_TReconstructedInfo;

typedef struct AL_TRecPic
{
  AL_TBuffer* pBuf;
  AL_TReconstructedInfo tInfo;
}AL_TRecPic;

/*@}*/
