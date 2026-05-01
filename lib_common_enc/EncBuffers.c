// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/****************************************************************************
   -----------------------------------------------------------------------------
 **************************************************************************//*!
   \addtogroup lib_base
   @{
   \file
 *****************************************************************************/

#include "EncBuffersInternal.h"

#include "lib_common/Utils.h"

#include "lib_common_enc/EncBuffers.h"
#include "lib_common_enc/EncSize.h"
#include "lib_common_enc/EncPicInfo.h"
#include "lib_common_enc/QPTableInternal.h"
#include "lib_rtos/message.h"

/****************************************************************************/
uint32_t AL_GetAllocSizeEP1(AL_ECodec eCodec)
{
  (void)eCodec;
  uint32_t uEP1Size = 0;
  uEP1Size += EP1_BUF_LAMBDAS.Size;

  if(AL_IS_ITU_CODEC(eCodec))
    uEP1Size += EP1_BUF_SCL_LST.Size;

  return RoundUp(uEP1Size, HW_IP_BURST_ALIGNMENT);
}

/****************************************************************************/
uint32_t AL_GetAllocSizeFlexibleEP2(AL_TDimension tDim, AL_ECodec eCodec, uint8_t uLog2MaxCuSize, uint8_t uQpLCUGranularity)
{
  return (uint32_t)(EP2_BUF_QP_CTRL.Size) + AL_QPTable_GetFlexibleSize(tDim, eCodec, uLog2MaxCuSize, uQpLCUGranularity);
}

/****************************************************************************/
uint32_t AL_GetAllocSizeEP2(AL_TDimension tDim, AL_ECodec eCodec, uint8_t uLog2MaxCuSize)
{
  return AL_GetAllocSizeFlexibleEP2(tDim, eCodec, uLog2MaxCuSize, 1);
}

/****************************************************************************/
uint32_t AL_GetAllocSizeEP3PerCore(void)
{
  return (uint32_t)(EP3_BUF_RC_TABLE1.Size + EP3_BUF_RC_TABLE2.Size + EP3_BUF_RC_CTX.Size + EP3_BUF_RC_LVL.Size);
}

/****************************************************************************/
uint32_t AL_GetAllocSizeEP3(void)
{
  uint32_t uMaxSize = AL_GetAllocSizeEP3PerCore() * AL_ENC_NUM_CORES;
  return RoundUp(uMaxSize, 128);
}

/****************************************************************************/
static uint32_t GetChromaAllocSize(AL_EChromaMode eChromaMode, uint32_t uAllocSizeY)
{
  switch(eChromaMode)
  {
  case AL_CHROMA_MONO: return 0;
  case AL_CHROMA_4_2_0: return uAllocSizeY / 2;
  case AL_CHROMA_4_2_2: return uAllocSizeY;
  case AL_CHROMA_4_4_4: return uAllocSizeY * 2;
  default: DBG_ASSERT_COND(0);
  }

  return 0;
}

int AL_EncGetMinPitch(int iWidth, AL_TPicFormat const* pPicFormat)
{
  return ComputeRndPitch(iWidth, pPicFormat, AL_ENC_PITCH_ALIGNMENT);
}

/****************************************************************************/
AL_EFbStorageMode AL_GetSrcStorageMode(AL_ESrcMode eSrcMode)
{
  switch(eSrcMode)
  {
  case AL_SRC_TILE_64x4:
  case AL_SRC_COMP_64x4:
    return AL_FB_TILE_64x4;
  case AL_SRC_TILE_32x4:
  case AL_SRC_COMP_32x4:
    return AL_FB_TILE_32x4;
  default:
    return AL_FB_RASTER;
  }
}

/****************************************************************************/
bool AL_IsSrcCompressed(AL_ESrcMode eSrcMode)
{
  (void)eSrcMode;
  bool bCompressed = false;
  return bCompressed;
}

/****************************************************************************/
bool AL_IsSrcInterleaved(AL_ESrcMode eSrcMode)
{
  (void)eSrcMode;
  bool bInterleaved = false;
  return bInterleaved;
}

/****************************************************************************/
bool AL_IsSrcMSB(AL_ESrcMode eSrcMode)
{
  (void)eSrcMode;
  bool bMSB = false;

  return bMSB;
}

/****************************************************************************/
uint32_t AL_GetAllocSizeSrc_PixPlane(AL_TPicFormat const* pPicFormat, int iPitch, int iStrideHeight, AL_EPlaneId ePlaneId)
{
  if(!AL_Plane_Exists(pPicFormat->ePlaneMode, false, ePlaneId))
    return 0;

  int iSize = iStrideHeight * iPitch / AL_GetNumLinesInPitch(pPicFormat->eStorageMode);

  if(ePlaneId == AL_PLANE_UV)
    iSize = GetChromaAllocSize(pPicFormat->eChromaMode, iSize);

  return RoundUp(iSize, HW_IP_BURST_ALIGNMENT);
}

/****************************************************************************/
/* Deprecated. Will be remove in 0.9 */
uint32_t AL_GetAllocSize_Src(AL_TDimension tDim, uint8_t uBitDepth, AL_EChromaMode eChromaMode, AL_ESrcMode eSrcFmt)
{
  AL_TPicFormat tPicFormat = GetDefaultPicFormat();
  tPicFormat.eChromaMode = eChromaMode;
  tPicFormat.ePlaneMode = GetInternalBufPlaneMode(eChromaMode);
  tPicFormat.uBitDepth = uBitDepth;
  tPicFormat.eStorageMode = AL_GetSrcStorageMode(eSrcFmt);
  tPicFormat.bCompressed = AL_GET_COMP_MODE(eSrcFmt);
  tPicFormat.bMSB = false;
  int const iPitch = AL_EncGetMinPitch(tDim.iWidth, &tPicFormat);
  return AL_GetAllocSizeSrc(tDim, &tPicFormat, iPitch, tDim.iHeight);
}

/****************************************************************************/
uint32_t AL_GetAllocSizeSrc(AL_TDimension tDim, AL_TPicFormat const* pPicFormat, int iPitch, int iStrideHeight)
{
  (void)tDim;

  AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES];
  uint32_t uSize = 0;
  int iPlanes = AL_Plane_GetBufferPlanes(*pPicFormat, usedPlanes);

  for(int iPlane = 0; iPlane < iPlanes; iPlane++)
  {
    uSize += AL_GetAllocSizeSrc_PixPlane(pPicFormat, iPitch, iStrideHeight, usedPlanes[iPlane]);
  }

  return uSize;
}

/****************************************************************************/
static uint32_t GetRasterFrameSize(AL_TDimension tDim, uint8_t uBitDepth, AL_EChromaMode eChromaMode)
{
  uint32_t uSize = tDim.iWidth * tDim.iHeight;
  uint32_t uSizeDiv = 1;
  switch(eChromaMode)
  {
  case AL_CHROMA_MONO: break;
  case AL_CHROMA_4_2_0:
  {
    uSize *= 3;
    uSizeDiv *= 2;
    break;
  }
  case AL_CHROMA_4_2_2:
  {
    uSize *= 2;
    break;
  }
  case AL_CHROMA_4_4_4:
  {
    uSize *= 3;
    break;
  }
  default:
    DBG_ASSERT_COND(0);
  }

  if(uBitDepth > 8)
  {
    DBG_ASSERT_COND((uBitDepth <= 12) && "Support bitpdeth > 12");
    uSize *= uBitDepth;
    uSizeDiv *= 8;
  }

  return uSize / uSizeDiv;
}

/****************************************************************************/
static uint32_t GetAllocSize_Ref(AL_TDimension tRoundedDim, uint8_t uBitDepth, AL_EChromaMode eChromaMode, uint16_t uMVVRange, uint8_t uLCUSize, AL_EChEncOption eOptions)
{
  (void)eOptions, (void)uMVVRange, (void)uLCUSize;
  AL_TDimension tDim = tRoundedDim;

  uint32_t uSize = GetRasterFrameSize(tDim, uBitDepth, eChromaMode);

  return uSize;
}

/****************************************************************************/
uint32_t AL_GetAllocSize_EncReference(AL_TDimension tDim, uint8_t uBitDepth, uint8_t uLCUSize, AL_EChromaMode eChromaMode, AL_EChEncOption eOptions, uint16_t uMVVRange)
{
  (void)uMVVRange, (void)uLCUSize;

  AL_TDimension RoundedDim;
  RoundedDim.iHeight = RoundUp(tDim.iHeight, 64);
  RoundedDim.iWidth = RoundUp(tDim.iWidth, 64);
  return GetAllocSize_Ref(RoundedDim, uBitDepth, eChromaMode, uMVVRange, uLCUSize, eOptions);
}

/****************************************************************************/
uint32_t AL_GetAllocSize_CompData(AL_TDimension tDim, uint8_t uLog2MaxCuSize, uint8_t uBitDepth, AL_EChromaMode eChromaMode, bool bUseEnt)
{
  uint32_t uBlk16x16 = GetSquareBlkNumber(tDim, 16);
  return AL_GetCompDataSize(uBlk16x16, uLog2MaxCuSize, uBitDepth, eChromaMode, bUseEnt);
}

/****************************************************************************/
uint32_t AL_GetAllocSize_EncCompMap(AL_TDimension tDim, uint8_t uLog2MaxCuSize, uint8_t uNumCore, bool bUseEnt)
{
  (void)uLog2MaxCuSize, (void)uNumCore, (void)bUseEnt;
  uint32_t uBlk16x16 = GetSquareBlkNumber(tDim, 16);
  return RoundUp(SIZE_LCU_INFO * uBlk16x16, 32);
}

/*****************************************************************************/
uint32_t AL_GetAllocSize_MV(AL_TDimension tDim, uint8_t uLog2MaxCuSize, AL_ECodec Codec)
{
  uint32_t uNumBlk = 0;
  int iMul = (Codec == AL_CODEC_HEVC) ? 1 :
             2;
  switch(uLog2MaxCuSize)
  {
  case 4: uNumBlk = GetSquareBlkNumber(tDim, 16);
    break;
  case 5: uNumBlk = GetSquareBlkNumber(tDim, 32) << 2;
    break;
  case 6: uNumBlk = GetSquareBlkNumber(tDim, 64) << 4;
    break;
  default: DBG_ASSERT_COND(0);
  }

  return MVBUFF_MV_OFFSET + ((uNumBlk * 4 * sizeof(uint32_t)) * iMul);
}

/*****************************************************************************/
uint32_t AL_GetAllocSize_WPP(int iLCUPicHeight, int iNumSlices, uint8_t uNumCore)
{
  uint32_t uNumLinesPerCmd = (((iLCUPicHeight + iNumSlices - 1) / iNumSlices) + uNumCore - 1) / uNumCore;
  uint32_t uAlignedSize = RoundUp(uNumLinesPerCmd * sizeof(uint32_t), 128) * uNumCore * iNumSlices;
  return uAlignedSize;
}

/*****************************************************************************/
uint32_t AL_GetAllocSize_SliceSize(uint32_t uWidth, uint32_t uHeight, uint32_t uNumSlices, uint32_t uLog2MaxCuSize)
{
  int iWidthInLcu = (uWidth + ((1 << uLog2MaxCuSize) - 1)) >> uLog2MaxCuSize;
  int iHeightInLcu = (uHeight + ((1 << uLog2MaxCuSize) - 1)) >> uLog2MaxCuSize;
  uint32_t uSize = (uint32_t)Max(iWidthInLcu * iHeightInLcu * 32, iWidthInLcu * iHeightInLcu * sizeof(uint32_t) + uNumSlices * AL_ENC_NUM_CORES * 128);
  uint32_t uAlignedSize = RoundUp(uSize, 32);
  return uAlignedSize;
}

/*****************************************************************************/
uint32_t GetAllocSize_StreamPart(AL_EProfile eProfile, int iNumCores, int iNumSlices, bool bSliceSize, int iNumTilesPerCore)
{
  (void)eProfile;

  int iMaxPart = bSliceSize ? AL_MAX_ENC_SLICE : iNumSlices;
  int iNumNal = 16;

  uint32_t uStreamPartSize = ((iMaxPart * iNumCores * iNumTilesPerCore) + iNumNal) * sizeof(AL_TStreamPart);
  uStreamPartSize = RoundUp(uStreamPartSize, 128);

  return uStreamPartSize;
}

/****************************************************************************/
static uint32_t GetRecPitch(uint32_t uBitDepth, uint32_t uWidth)
{
  int iTileWidth = 64;
  int iTileHeight = 4;

  if(uBitDepth == 8)
    return UnsignedRoundUp(uWidth, iTileWidth) * iTileHeight;

  return UnsignedRoundUp(uWidth, iTileWidth) * iTileHeight * uBitDepth / 8;
}

/****************************************************************************/
void AL_FillPlaneDesc_EncReference(AL_TPlaneDescription* pPlaneDesc, AL_TDimension tDim, AL_EChromaMode eChromaMode, uint8_t uBitDepth, bool bIsAvc, uint8_t uLCUSize, uint16_t uMVVRange, AL_EChEncOption eOptions)
{
  (void)eChromaMode, (void)bIsAvc; // if no fbc support
  AL_EChEncOption eTmpOption = eOptions;

  if(AL_Plane_IsPixelPlane(pPlaneDesc->ePlaneId))
  {
    pPlaneDesc->iPitch = GetRecPitch(uBitDepth, tDim.iWidth);
    pPlaneDesc->iOffset = 0;

    if(pPlaneDesc->ePlaneId != AL_PLANE_Y)
    {
      int iPlaneOrder = pPlaneDesc->ePlaneId == AL_PLANE_V ? 2 : 1;
      pPlaneDesc->iOffset = iPlaneOrder * AL_GetAllocSize_EncReference(tDim, uBitDepth, uLCUSize, AL_CHROMA_MONO, eTmpOption, uMVVRange);
    }

    return;
  }

  DBG_ASSERT_COND(0);
}

/*!@}*/
