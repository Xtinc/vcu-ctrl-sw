// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/****************************************************************************
   -----------------------------------------------------------------------------
 **************************************************************************//*!
   \addtogroup lib_decode_hls
   @{
   \file
 *****************************************************************************/

#pragma once

#include "lib_common/PicFormat.h"
#include "lib_decode/lib_decode.h"

typedef struct AL_IDecArchVtable
{
  void (* Deinit)(void);
  AL_ERR (* DecoderCreate)(AL_HDecoder* hDec, void* pScheduler, AL_TAllocator* pAllocator, void* pSettings, void* pCB);
  void (* DecoderDestroy)(AL_HDecoder hDec);
  void (* DecoderSetParam)(AL_HDecoder hDec, const char* sPrefix, int iFrmID, int iNumFrm, bool bForceCleanBuffers, bool bShouldPrintFrameDelimiter);
  bool (* DecoderPushStreamBuffer)(AL_HDecoder hDec, AL_TBuffer* pBuf, size_t uSize, uint8_t uFlags);
  bool (* DecoderPushBuffer)(AL_HDecoder hDec, AL_TBuffer* pBuf, size_t uSize);
  void (* DecoderFlush)(AL_HDecoder hDec);
  bool (* DecoderPutDisplayPicture)(AL_HDecoder hDec, AL_TBuffer* pDisplay);
  AL_ECodec (* DecoderGetCodec)(AL_HDecoder hDec);
  int (* DecoderGetMaxBD)(AL_HDecoder hDec);
  AL_ERR (* DecoderGetLastError)(AL_HDecoder hDec);
  AL_ERR (* DecoderGetFrameError)(AL_HDecoder hDec, AL_TBuffer const* pBuf);
  bool (* DecoderPreallocateBuffers)(AL_HDecoder hDec);
  int32_t (* DecoderGetMinPitch)(int32_t iWidth, AL_TPicFormat const* pPicFormat);
  int32_t (* DecoderGetMinStrideHeight)(int32_t iHeight, AL_TPicFormat const* pPicFormat);
  bool (* DecoderSetDecOutputSettings)(AL_HDecoder hDec, AL_TDecOutputSettings const* pDecOutputSettings);
}AL_IDecArchVtable;

typedef struct AL_IDecArch
{
  AL_IDecArchVtable const* vtable;
}AL_IDecArch;

/*@}*/
