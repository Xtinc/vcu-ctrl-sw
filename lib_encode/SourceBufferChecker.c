// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "SourceBufferChecker.h"
#include "lib_common/PixMapBufferInternal.h"
#include "lib_common_enc/EncBuffers.h"
#include "lib_common_enc/IpEncFourCC.h"
#include "lib_common/Utils.h"
#include "lib_rtos/message.h"
#include "config.h"

void AL_SrcBuffersChecker_Init(AL_TSrcBufferChecker* pCtx, AL_TEncChanParam const* pChParam)
{
  pCtx->maxDim.iWidth = AL_GetSrcWidth(*pChParam);
  pCtx->maxDim.iHeight = AL_GetSrcHeight(*pChParam);

  pCtx->currentDim = pCtx->maxDim;

  AL_EChromaMode eChromaMode = AL_GET_CHROMA_MODE(pChParam->ePicFormat);

  AL_TPicFormat tPicFormat = AL_EncGetSrcPicFormat(eChromaMode, pChParam->uSrcBitDepth, pChParam->eSrcMode);
  pCtx->fourCC = AL_GetFourCC(tPicFormat);
  pCtx->bMonochrome = tPicFormat.eChromaMode == AL_CHROMA_MONO;
  pCtx->srcMode = pChParam->eSrcMode;
}

bool AL_SrcBuffersChecker_UpdateResolution(AL_TSrcBufferChecker* pCtx, AL_TDimension tNewDim)
{
  if((pCtx->maxDim.iWidth < tNewDim.iWidth) || (pCtx->maxDim.iHeight < tNewDim.iHeight))
    return false;
  pCtx->currentDim = tNewDim;
  return true;
}

static TFourCC GetMonochromeFourCC(TFourCC tFourCC)
{
  AL_TPicFormat tPicFmt;
  AL_GetPicFormat(tFourCC, &tPicFmt);
  tPicFmt.eChromaMode = AL_CHROMA_4_0_0;
  tPicFmt.ePlaneMode = AL_PLANE_MODE_MONOPLANE;
  return AL_GetFourCC(tPicFmt);
}

static bool CheckMetaData(AL_TSrcBufferChecker* pCtx, AL_TBuffer* pBuf)
{
  if(AL_Buffer_GetMetaData(pBuf, AL_META_TYPE_PIXMAP) == NULL)
    return false;

  AL_TDimension tDim = AL_PixMapBuffer_GetDimension(pBuf);
  TFourCC tFourCC = AL_PixMapBuffer_GetFourCC(pBuf);

  if(tDim.iWidth != pCtx->currentDim.iWidth)
    return false;

  if(tDim.iHeight != pCtx->currentDim.iHeight)
    return false;

  /*
    We can inject buffer with chroma for monochrome encoding. This assumes encoder sources
    always have luma and chroma in different planes.
  */
  bool bValidFormat = tFourCC == pCtx->fourCC ||
                      (pCtx->bMonochrome && GetMonochromeFourCC(tFourCC) == pCtx->fourCC);
  return bValidFormat;
}

static uint32_t GetSrcPlaneSize(AL_TDimension tDim, AL_EChromaMode eChromaMode, AL_ESrcMode eSrcFmt, int iPitchY, int iStrideHeight, AL_EPlaneId ePlaneId)
{
  (void)tDim;
  AL_TPicFormat tPicFormat = AL_EncGetSrcPicFormat(eChromaMode, 8, eSrcFmt);

  if(AL_Plane_IsPixelPlane(ePlaneId))
    return AL_GetAllocSizeSrc_PixPlane(&tPicFormat, iPitchY, iStrideHeight, ePlaneId);

  return 0;
}

static bool CheckPlanes(AL_TSrcBufferChecker* pCtx, AL_TBuffer* pBuf)
{
  TFourCC tFourCC = AL_PixMapBuffer_GetFourCC(pBuf);
  AL_TDimension tDim = AL_PixMapBuffer_GetDimension(pBuf);
  AL_TPicFormat tPicFormat;
  bool const bSuccess = AL_GetPicFormat(tFourCC, &tPicFormat);

  if(!bSuccess)
  {
    DBG_ASSERT_COND(bSuccess);
    return false;
  }

  AL_EPlaneId ePlaneId = AL_PLANE_Y;

  const int iMinPitch = AL_EncGetMinPitch(tDim.iWidth, &tPicFormat);
  int const iMinStrideHeight = RoundUp(tDim.iHeight, 8);

  int const iPitchY = AL_PixMapBuffer_GetPlanePitch(pBuf, ePlaneId);

  if(iPitchY < iMinPitch || (iPitchY % HW_IP_BURST_ALIGNMENT != 0))
    return false;

  uint32_t uChunkSizes[AL_BUFFER_MAX_CHUNK] = { 0 };
  AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES];
  int const iNbPlanes = AL_Plane_GetBufferPixelPlanes(tPicFormat, usedPlanes);

  for(int iPlane = 0; iPlane < iNbPlanes; iPlane++)
  {
    AL_EPlaneId ePlaneId = usedPlanes[iPlane];

    int iChunkIdx = AL_PixMapBuffer_GetPlaneChunkIdx(pBuf, ePlaneId);
    DBG_ASSERT_COND(iChunkIdx != AL_BUFFER_BAD_CHUNK);

    if(AL_Plane_IsPixelPlane(ePlaneId) && ePlaneId != AL_PLANE_Y && ePlaneId != AL_PLANE_YUV)
    {
      int const iPitch = AL_PixMapBuffer_GetPlanePitch(pBuf, ePlaneId);

      if(iPitch != AL_GetChromaPitch(tFourCC, iPitchY))
        return false;
    }

    uChunkSizes[iChunkIdx] += GetSrcPlaneSize(tDim, tPicFormat.eChromaMode, pCtx->srcMode, iPitchY, iMinStrideHeight, ePlaneId);
  }

  for(int i = 0; i < AL_BUFFER_MAX_CHUNK; i++)
  {
    if(uChunkSizes[i] != 0 && (AL_Buffer_GetSizeChunk(pBuf, i) < uChunkSizes[i]))
      return false;
  }

  return true;
}

bool AL_SrcBuffersChecker_CanBeUsed(AL_TSrcBufferChecker* pCtx, AL_TBuffer* pBuf)
{
  if(pBuf == NULL)
    return false;

  if(!CheckMetaData(pCtx, pBuf))
    return false;

  return CheckPlanes(pCtx, pBuf);
}
