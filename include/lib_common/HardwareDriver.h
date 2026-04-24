// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/**************************************************************************//*!
   \addtogroup Driver
   @{
   \file
 *****************************************************************************/
#pragma once

typedef struct AL_TDriver AL_TDriver;

/*************************************************************************//*!
    \brief Get a driver that will access an hardware device
*****************************************************************************/
AL_TDriver* AL_GetHardwareDriver(void);

/*@}*/
