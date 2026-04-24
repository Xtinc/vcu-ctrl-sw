// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

typedef enum AL_ECodecHook
{
  AL_HOOK_ENC,
  AL_HOOK_ENC1,
  AL_HOOK_ENC2,
  AL_HOOK_ENCJPEG,
  AL_HOOK_SCD,
  AL_HOOK_DEC1,
  AL_HOOK_DEC2,
  AL_HOOK_JPEG,
  AL_HOOK_FBC,
  AL_HOOK_FBD,
  AL_HOOK_ME,
  AL_HOOK_POSTPROC,
  AL_HOOK_BITSTREAM_FLUSH,
  AL_HOOK_LCEVC_DEA,
  AL_HOOK_LCEVC_DEC,
  AL_HOOK_LCEVC_ENC_PREPRO,
  AL_HOOK_LCEVC_ENC_LOQ,
  AL_HOOK_LCEVC_ENC_ENT,
  AL_HOOK_TOP,
}AL_ECodecHook;
