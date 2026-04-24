// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_rtos/lib_rtos.h"
#include "lib_common_dec/ParseResult.h"

typedef struct t_Conceal
{
  bool bHasPPS;
  bool bValidFrame;
  int iLastPPSId;
  int iActivePPS;
  int iFirstLCU;
  bool bSkipRemainingNals;
}AL_TConceal;

void AL_Conceal_Init(AL_TConceal* pConceal);
