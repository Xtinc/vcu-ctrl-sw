// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once
#include "lib_rtos/lib_rtos.h"

/*************************************************************************/
typedef struct AL_TLevelLimit
{
  uint64_t uLimit;
  uint8_t uLevel;
}AL_TLevelLimit;

/*************************************************************************/
uint8_t AL_GetRequiredLevel(uint32_t uVal, const AL_TLevelLimit* pLevelLimits, int iNbLimits);
