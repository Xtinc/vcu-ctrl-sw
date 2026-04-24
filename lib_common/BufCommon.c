// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "Utils.h"

#include "lib_rtos/lib_rtos.h"
#include "lib_common/BufCommon.h"
#include "lib_common/BufferPixMapMeta.h"

#include "lib_rtos/message.h"
#include "config.h"

/*****************************************************************************/
int AL_GetNumLinesInPitch(AL_EFbStorageMode eFrameBufferStorageMode)
{
  switch(eFrameBufferStorageMode)
  {
  case AL_FB_RASTER: return 1;
  case AL_FB_TILE_32x4:
  case AL_FB_TILE_64x4: return 4;
  default:
  {
    DBG_ASSERT_COND(false);
    return 0;
  }
  }
}

/******************************************************************************/
static inline int GetWidthRound(AL_EFbStorageMode eStorageMode, uint8_t uBitDepth)
{
  (void)uBitDepth;
  switch(eStorageMode)
  {
  case AL_FB_RASTER: return 1;
  case AL_FB_TILE_64x4: return 64;
  case AL_FB_TILE_32x4: return 32;
  default:
  {
    DBG_ASSERT_COND(false);
    return 0;
  }
  }
}

/******************************************************************************/
int32_t ComputeRndPitch(int32_t iWidth, AL_TPicFormat const* pPicFormat, int iBurstAlignment)
{
  int32_t iVal = 0;
  int const iRndWidth = RoundUp(iWidth, GetWidthRound(pPicFormat->eStorageMode, pPicFormat->uBitDepth));
  switch(pPicFormat->eStorageMode)
  {
  case AL_FB_RASTER:
  {
    if(pPicFormat->ePlaneMode == AL_PLANE_MODE_INTERLEAVED && pPicFormat->eChromaMode != AL_CHROMA_4_0_0)
    {
      int iPixSize = sizeof(uint32_t);
      int iHorizontalScale = pPicFormat->eChromaMode == AL_CHROMA_4_4_4 ? 1 : 2;

      bool bHasAlpha = pPicFormat->eAlphaMode == AL_ALPHA_MODE_BEFORE || pPicFormat->eAlphaMode == AL_ALPHA_MODE_AFTER;

      /* This checks mainly AYUV and Y410 formats*/
      if((bHasAlpha && pPicFormat->uBitDepth == 8)
         || (pPicFormat->eSamplePackMode == AL_SAMPLE_PACK_MODE_PACKED && pPicFormat->uBitDepth == 10))
        iPixSize = sizeof(uint32_t);

      /* This checks mainly Y416 format*/
      if(pPicFormat->eSamplePackMode == AL_SAMPLE_PACK_MODE_BYTE && (pPicFormat->uBitDepth == 12 || pPicFormat->uBitDepth == 10))
        iPixSize = sizeof(AL_64U);
      iVal = iRndWidth * iPixSize / iHorizontalScale;
      break;
    }

    if(pPicFormat->uBitDepth == 8)
      iVal = iRndWidth;
    else
    {
      iVal = iRndWidth * 2;

      if(pPicFormat->eSamplePackMode == AL_SAMPLE_PACK_MODE_PACKED_XV)
        iVal = (iRndWidth + 2) / 3 * 4;
    }
    break;
  }
  case AL_FB_TILE_32x4:
  case AL_FB_TILE_64x4:
  {
    uint8_t uBitDepth = (pPicFormat->uBitDepth + 1) & 0xFE; // Prevent 9 and 11 bitdepth -> 10/12
    iVal = iRndWidth * AL_GetNumLinesInPitch(pPicFormat->eStorageMode) * uBitDepth / 8;
    break;
  }
  default:
    DBG_ASSERT_COND(false);
  }

  DBG_ASSERT_COND(iBurstAlignment > 0);
  DBG_ASSERT_COND((iBurstAlignment % HW_IP_BURST_ALIGNMENT) == 0);
  return RoundUp(iVal, iBurstAlignment);
}

/****************************************************************************/
int AL_GetChromaPitch(TFourCC tFourCC, int iLumaPitch)
{
  AL_TPicFormat tPicFormat;
  bool const bSuccess = AL_GetPicFormat(tFourCC, &tPicFormat);

  if(!bSuccess)
    return -1;

  if(tPicFormat.eChromaMode == AL_CHROMA_MONO)
    return 0;

  int iNumPlanes = tPicFormat.ePlaneMode == AL_PLANE_MODE_SEMIPLANAR ? 2 : 1;
  int iChromaPitch = iLumaPitch;

  if(tPicFormat.eChromaMode != AL_CHROMA_4_4_4)
  {
    int iRound = tPicFormat.uBitDepth > 8 ? 4 : 2;
    iChromaPitch = RoundUp(iLumaPitch, iRound) / 2;
  }

  return iChromaPitch * iNumPlanes;
}

/****************************************************************************/
int AL_GetChromaWidth(TFourCC tFourCC, int iLumaWidth)
{
  AL_TPicFormat tPicFormat;
  bool const bSuccess = AL_GetPicFormat(tFourCC, &tPicFormat);

  if(!bSuccess)
    return -1;

  if(tPicFormat.eChromaMode == AL_CHROMA_MONO)
    return 0;

  int iNumPlanes = tPicFormat.ePlaneMode == AL_PLANE_MODE_SEMIPLANAR ? 2 : 1;
  int iHrzScale = tPicFormat.eChromaMode == AL_CHROMA_4_4_4 ? 1 : 2;
  return ((iLumaWidth + iHrzScale - 1) / iHrzScale) * iNumPlanes;
}

/****************************************************************************/
int AL_GetChromaHeight(TFourCC tFourCC, int iLumaHeight)
{
  AL_EChromaMode eChromaMode = AL_GetChromaMode(tFourCC);

  if(eChromaMode == AL_CHROMA_MAX_ENUM)
    return -1;

  if(eChromaMode == AL_CHROMA_MONO)
    return 0;

  return eChromaMode == AL_CHROMA_4_2_0 ? (iLumaHeight + 1) / 2 : iLumaHeight;
}

/****************************************************************************/
int AL_CLEAN_BUFFERS = 0;

void AL_CleanupMemory(void* pDst, size_t uSize)
{
  if(!pDst)
    return;

  if(AL_CLEAN_BUFFERS)
  {
    Rtos_Memset(pDst, 0, uSize);
#ifdef COMPILE_FOR_MCU
    Rtos_FlushCacheMemory(pDst, uSize);
#endif
  }
}
