// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "lib_common/PicFormat.h"

/****************************************************************************/
/* This function is intended to be used only for internal buffers, which cannot exist
  in packed format.
  Internal buffers configured with 420 & 422 chroma modes are stored in interleaved.
*/
AL_EPlaneMode GetInternalBufPlaneMode(AL_EChromaMode eChromaMode)
{
  return eChromaMode == AL_CHROMA_MONO ? AL_PLANE_MODE_MONOPLANE :
         (eChromaMode == AL_CHROMA_4_4_4 ? AL_PLANE_MODE_PLANAR : AL_PLANE_MODE_SEMIPLANAR);
}

/****************************************************************************/
AL_ESamplePackMode GetInternalBufSamplePackMode(AL_EFbStorageMode eFbStorageMode, uint8_t uBitDepth)
{
  (void)uBitDepth;

  if(IsTile(eFbStorageMode))
    return AL_SAMPLE_PACK_MODE_PACKED;

  return uBitDepth == 10 ? AL_SAMPLE_PACK_MODE_PACKED_XV : AL_SAMPLE_PACK_MODE_BYTE;
}

/*****************************************************************************/
bool IsTile(AL_EFbStorageMode eStorageMode)
{
  bool bIsTile = AL_FB_TILE_32x4 == eStorageMode || AL_FB_TILE_64x4 == eStorageMode;
  return bIsTile;
}

/*****************************************************************************/
int GetTileWidth(AL_EFbStorageMode eMode, uint8_t uBitDepth)
{
  (void)uBitDepth;

  if(eMode == AL_FB_TILE_32x4
     )
    return 32;

  if(eMode == AL_FB_TILE_64x4
     )
    return 64;

  return 0;
}

/*****************************************************************************/
int GetTileHeight(AL_EFbStorageMode eMode)
{
  if(eMode == AL_FB_TILE_32x4 || eMode == AL_FB_TILE_64x4)
    return 4;

  return 0;
}

/*****************************************************************************/
int GetTileSize(AL_EFbStorageMode eMode, uint8_t uBitDepth)
{
  return GetTileHeight(eMode) * GetTileWidth(eMode, uBitDepth) * uBitDepth / 8;
}

/*****************************************************************************/

bool IsRgbComponentOrder(AL_EComponentOrder eComponentOrder)
{
  return eComponentOrder >= AL_COMPONENT_ORDER_RGB && eComponentOrder <= AL_COMPONENT_ORDER_BGR;
}

/*****************************************************************************/
AL_TPicFormat GetDefaultPicFormat(void)
{
  AL_TPicFormat picFormat =
  {
    /* MAX_ENUM are filled in later */
    AL_CHROMA_MAX_ENUM,
    AL_ALPHA_MODE_DISABLED,
    0,
    AL_FB_RASTER,
    AL_PLANE_MODE_MAX_ENUM,
    AL_COMPONENT_ORDER_YUV,
    AL_SAMPLE_PACK_MODE_BYTE,
    false,
    false,
  };
  return picFormat;
}
