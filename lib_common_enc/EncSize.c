// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "EncSize.h"
#include "lib_common/StreamBufferPrivate.h"

/****************************************************************************/
int32_t AL_GetCompDataSize(int32_t iNumLCU, uint8_t uLog2MaxCUSize, uint8_t uBitDepth, AL_EChromaMode eChromaMode, bool bUseEnt)
{

  (void)iNumLCU, (void)uLog2MaxCUSize, (void)uBitDepth, (void)eChromaMode, (void)bUseEnt;
  // header + MVDs + residuals words size
  return iNumLCU * 1312;
}

/****************************************************************************/
