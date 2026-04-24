// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include <iostream>
#include <algorithm>
#include <assert.h>

#include "lib_app/UnCompFrameWriter.h"

extern "C"
{
#include "lib_common/PixMapBuffer.h"
#include "lib_common/Planes.h"
#include "lib_common/DisplayInfoMeta.h"
}

using namespace std;

/****************************************************************************/
UnCompFrameWriter::UnCompFrameWriter(std::shared_ptr<std::ostream> recFile, AL_EFbStorageMode eStorageMode) :
  BaseFrameWriter(recFile, eStorageMode)
{
}

/****************************************************************************/
void UnCompFrameWriter::WriteFrame(AL_TBuffer* pBuf, AL_TCropInfo* pCrop, AL_EPicStruct ePicStruct)
{
  (void)ePicStruct;

  AL_TCropInfo tCrop {};

  if(pCrop)
    tCrop = *pCrop;

  m_tFourCC = AL_PixMapBuffer_GetFourCC(pBuf);
  AL_GetPicFormat(m_tFourCC, &m_tPicFormat);

  AL_EFbStorageMode currentStorageMode = AL_GetStorageMode(m_tFourCC);

  if(currentStorageMode != m_eStorageMode)
    return;

  m_tPicDim = AL_PixMapBuffer_GetDimension(pBuf);

  int32_t iPitchInLuma = 0;
  int32_t iPitchInChroma = 0;

  uint8_t* pY = nullptr;
  uint8_t* pC1 = nullptr;
  uint8_t* pC2 = nullptr;

  if(m_tPicFormat.ePlaneMode == AL_PLANE_MODE_INTERLEAVED)
  {
    pY = AL_PixMapBuffer_GetPlaneAddress(pBuf, AL_PLANE_YUV);
    iPitchInLuma = AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_YUV);
  }
  else
  {
    pY = AL_PixMapBuffer_GetPlaneAddress(pBuf, AL_PLANE_Y);
    iPitchInLuma = AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_Y);

    if(m_tPicFormat.ePlaneMode == AL_PLANE_MODE_PLANAR && m_tPicFormat.eChromaMode != AL_CHROMA_4_0_0)
    {
      pC1 = AL_PixMapBuffer_GetPlaneAddress(pBuf, AL_PLANE_U);
      pC2 = AL_PixMapBuffer_GetPlaneAddress(pBuf, AL_PLANE_V);
      iPitchInChroma = AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_U);

      if(int(iPitchInChroma) != AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_V))
        throw std::runtime_error(ErrorMessagePitch);
    }
    else if(m_tPicFormat.ePlaneMode == AL_PLANE_MODE_SEMIPLANAR)
    {
      pC1 = AL_PixMapBuffer_GetPlaneAddress(pBuf, AL_PLANE_UV);
      iPitchInChroma = AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_UV);
    }
  }

  if(tCrop.bCropping)
  {
    m_tPicDim.iWidth -= tCrop.uCropOffsetLeft + tCrop.uCropOffsetRight;
    m_tPicDim.iHeight -= tCrop.uCropOffsetTop + tCrop.uCropOffsetBottom;
  }

  if(AL_IsTiled(m_tFourCC))
    DimInTileCalculus();
  else
    DimInTileCalculusRaster();

  if(tCrop.bCropping)
    pY += tCrop.uCropOffsetTop * iPitchInLuma + tCrop.uCropOffsetLeft * m_iNbBytesPerPix;

  WritePix(pY, iPitchInLuma, m_uHeightInTileYFile, m_uPitchYFile);

  if(m_tPicFormat.eChromaMode != AL_CHROMA_4_0_0)
  {
    tCrop.uCropOffsetTop /= m_tPicFormat.eChromaMode == AL_CHROMA_4_2_0 ? 2 : 1;
    tCrop.uCropOffsetLeft /= m_tPicFormat.eChromaMode == AL_CHROMA_4_4_4 ? 1 : 2;

    if(m_tPicFormat.ePlaneMode == AL_PLANE_MODE_PLANAR)
    {
      if(tCrop.bCropping)
      {
        pC1 += tCrop.uCropOffsetTop * iPitchInChroma + tCrop.uCropOffsetLeft * m_iNbBytesPerPix;
        pC2 += tCrop.uCropOffsetTop * iPitchInChroma + tCrop.uCropOffsetLeft * m_iNbBytesPerPix;
      }

      WritePix(pC1, iPitchInChroma, m_uHeightInTileCFile, m_uPitchCFile);
      WritePix(pC2, iPitchInChroma, m_uHeightInTileCFile, m_uPitchCFile);
    }
    else if(m_tPicFormat.ePlaneMode == AL_PLANE_MODE_SEMIPLANAR)
    {
      if(tCrop.bCropping)
        pC1 += tCrop.uCropOffsetTop * iPitchInChroma + tCrop.uCropOffsetLeft * m_iNbBytesPerPix * 2;

      WritePix(pC1, iPitchInChroma, m_uHeightInTileCFile, m_uPitchCFile);
    }
  }
}

/****************************************************************************/
void UnCompFrameWriter::DimInTileCalculusRaster(void)
{
  FactorsCalculus();

  m_uPitchYFile = m_tPicDim.iWidth * m_iNbBytesPerPix;
  m_uHeightInTileYFile = m_tPicDim.iHeight;
  m_uHeightInTileCFile = AL_RoundUp(m_uHeightInTileYFile, m_iChromaVertScale) / m_iChromaVertScale;

  if(m_tPicFormat.ePlaneMode == AL_PLANE_MODE_SEMIPLANAR)
    m_uPitchCFile = m_uPitchYFile;
  else
    m_uPitchCFile = ((m_tPicDim.iWidth + m_iChromaHorzScale - 1) / m_iChromaHorzScale) * m_iNbBytesPerPix;
}
