// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_common_dec/StreamSettings.h"
#include <stdbool.h>

/******************************************************************************/
int AL_AVC_GetMaxDpbBuffers(AL_TStreamSettings const* pCurrentStreamSettings, int iSPSMaxRefFrames);

/******************************************************************************/
int AVC_GetMinOutputBuffersNeeded(int iDpbMaxBuf, int iStack, bool bDecodeIntraOnly);

/*****************************************************************************/
int AL_HEVC_GetMaxDpbBuffers(AL_TStreamSettings const* pCurrentStreamSettings);
