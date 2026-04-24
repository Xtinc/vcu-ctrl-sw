// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/**************************************************************************//*!
   \addtogroup Buffers
   @{
   \file
 *****************************************************************************/

#pragma once

#include "lib_common/BufferMeta.h"

/*************************************************************************//*!
   \brief Useful information about the bitstream choices for the frame
*****************************************************************************/
typedef struct AL_TPictureDecMetaData
{
  AL_TMetaData tMeta;
  bool bLastFrameFromInputPayload; /*!< picture is the last frame from the input payload, used for split-input */
}AL_TPictureDecMetaData;

/*************************************************************************//*!
   \brief Create a decoder picture metadata.
   The last frame defaults to true
*****************************************************************************/
AL_TPictureDecMetaData* AL_PictureDecMetaData_Create(void);
AL_TPictureDecMetaData* AL_PictureDecMetaData_Clone(AL_TPictureDecMetaData* pMeta);

/*@}*/
