// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_common_dec/I_Feeder.h"

AL_TFeeder* AL_UnsplitBufferFeeder_Create(AL_HANDLE hDec, int uMaxBufNum, AL_TAllocator* pAllocator, int iBufferStreamSize, AL_TBuffer* eosBuffer, bool bForceAccessUnitDestroy);
