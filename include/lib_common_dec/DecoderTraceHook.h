// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_common/CodecHook.h"

typedef struct AL_TDecTraceHook
{
  void* pUserParam;
  void (* RecordStart)(void*, AL_ECodecHook, int, int, int, int, bool);
  void (* RecordEnd)(void*, AL_ECodecHook, int, int, int, int, bool);
  void (* SetActiveWorker)(void*, AL_ECodecHook, int, int);
  void (* BeginTraces)(void*);
  void (* EndTraces)(void*);
}AL_TDecTraceHook;
