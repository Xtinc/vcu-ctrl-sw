// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/**************************************************************************//*!
   \addtogroup Decoder_Settings
   @{
   \file
 *****************************************************************************/
#pragma once

#include "lib_common/PicFormat.h"

/*************************************************************************//*!
   \brief Decoder frame output configuration
*****************************************************************************/
typedef struct AL_TDecOutputSettings
{
  AL_TPicFormat tPicFormat; /*!< Output frame format parameters */
}AL_TDecOutputSettings;

void SetDefaultDecOutputSettings(AL_TDecOutputSettings* pDecOutputSettings);
/*@}*/
