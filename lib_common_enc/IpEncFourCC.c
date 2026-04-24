// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "lib_common_enc/IpEncFourCC.h"

#include "lib_common/FourCC.h"
#include "lib_rtos/message.h"
#include "lib_common_enc/EncBuffers.h"

/****************************************************************************/
TFourCC AL_EncGetSrcFourCC(AL_TPicFormat const picFmt)
{
  if(AL_FB_RASTER == picFmt.eStorageMode)
  {
    DBG_ASSERT_COND(picFmt.eChromaMode == AL_CHROMA_MONO
              || picFmt.ePlaneMode == AL_PLANE_MODE_SEMIPLANAR
              || (picFmt.eChromaMode == AL_CHROMA_4_4_4 && picFmt.ePlaneMode == AL_PLANE_MODE_PLANAR));
    DBG_ASSERT_COND(picFmt.uBitDepth == 8 || (AL_SAMPLE_PACK_MODE_PACKED_XV == picFmt.eSamplePackMode));
  }

  return AL_GetFourCC(picFmt);
}

/****************************************************************************/
AL_TPicFormat AL_EncGetSrcPicFormat(AL_EChromaMode eChromaMode, uint8_t uBitDepth, AL_ESrcMode eSrcMode)
{
  AL_ESamplePackMode eSamplePackMode = AL_SAMPLE_PACK_MODE_BYTE;
  bool bIsSourceRGB = false;
  AL_EAlphaMode eAlphaMode = AL_ALPHA_MODE_DISABLED;
  AL_EChromaMode eRealChromaMode = eChromaMode;

  AL_EComponentOrder eComponentOrder = bIsSourceRGB ? AL_COMPONENT_ORDER_BGR : AL_COMPONENT_ORDER_YUV;
  AL_EFbStorageMode eStorageMode = AL_GetSrcStorageMode(eSrcMode);

  bool bCompressed = false;

  bool bMSB = false;

  bool bInterleaved = false;

  if(eStorageMode == AL_FB_TILE_32x4 || eStorageMode == AL_FB_TILE_64x4)
    eSamplePackMode = AL_SAMPLE_PACK_MODE_PACKED;

  if(AL_FB_RASTER == eStorageMode && 10 == uBitDepth)
  {
    eSamplePackMode = AL_SAMPLE_PACK_MODE_PACKED_XV;
    bMSB = false;
  }

  AL_TPicFormat picFormat =
  {
    eRealChromaMode,
    eAlphaMode,
    uBitDepth,
    eStorageMode,
    bInterleaved ? AL_PLANE_MODE_INTERLEAVED : GetInternalBufPlaneMode(eChromaMode),
    eComponentOrder,
    eSamplePackMode,
    bCompressed,
    bMSB
  };
  return picFormat;
}

/****************************************************************************/
TFourCC AL_GetRecFourCC(AL_TPicFormat const picFmt)
{
  DBG_ASSERT_COND(picFmt.eStorageMode == AL_FB_TILE_64x4);
  return AL_GetFourCC(picFmt);
}

/****************************************************************************/
AL_TPicFormat AL_EncGetRecPicFormat(AL_EChromaMode eChromaMode, uint8_t uBitDepth, bool bIsCompressed)
{
  AL_TPicFormat picFormat =
  {
    eChromaMode,
    AL_ALPHA_MODE_DISABLED,
    uBitDepth,
    AL_FB_TILE_64x4,
    GetInternalBufPlaneMode(eChromaMode),
    AL_COMPONENT_ORDER_YUV,
    AL_SAMPLE_PACK_MODE_PACKED,
    bIsCompressed,
    false
  };
  return picFormat;
}
