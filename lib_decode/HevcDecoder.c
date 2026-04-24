// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "FrameParam.h"
#include "I_DecoderCtx.h"
#include "DefaultDecoder.h"
#include "SliceDataParsing.h"
#include "NalUnitParserPrivate.h"

#include "lib_common/Nuts.h"
#include "lib_common/SliceHeader.h"
#include "lib_common/Utils.h"
#include "lib_common/HevcLevelsLimit.h"
#include "lib_common/HevcUtils.h"
#include "lib_common/Error.h"
#include "lib_common/SyntaxConversion.h"

#include "lib_common_dec/RbspParser.h"
#include "lib_common_dec/DecInfo.h"
#include "lib_common_dec/DecInfoInternal.h"
#include "lib_common_dec/Defines_mcu.h"
#include "lib_common_dec/DecHardwareConfig.h"
#include "lib_common_dec/StreamSettingsInternal.h"
#include "lib_decode/NalUnitParser.h"

#include "lib_common_dec/HDRMeta.h"

#include "lib_decode/I_DecSchedulerInfo.h"

#include "lib_parsing/HevcParser.h"
#include "lib_parsing/Hevc_PictMngr.h"
#include "lib_parsing/Hevc_SliceHeaderParsing.h"

#include "lib_rtos/message.h"

/*************************************************************************/
/*
static uint8_t getMaxRextBitDepth(AL_THevcProfilevel pf)
{
  if(pf.general_max_8bit_constraint_flag)
    return 8;

  if(pf.general_max_10bit_constraint_flag)
    return 10;

  if(pf.general_max_12bit_constraint_flag)
    return 12;
  return 16;
}
*/
/*************************************************************************/
/*
static int getMaxBitDepthFromSPSProfile(AL_THevcSps const* pSPS)
{
  AL_THevcProfilevel pf = pSPS->profile_and_level;

  if(pf.general_profile_idc == AL_GET_PROFILE_IDC(AL_PROFILE_HEVC_RExt))
    return getMaxRextBitDepth(pf);

  if((pf.general_profile_idc == AL_GET_PROFILE_IDC(AL_PROFILE_HEVC_MAIN)) || (pf.general_profile_compatibility_flag[AL_GET_PROFILE_IDC(AL_PROFILE_HEVC_MAIN)] == 1))
    return 8;

  if((pf.general_profile_idc == AL_GET_PROFILE_IDC(AL_PROFILE_HEVC_MAIN_STILL)) || (pf.general_profile_compatibility_flag[AL_GET_PROFILE_IDC(AL_PROFILE_HEVC_MAIN_STILL)] == 1))
    return 8;

  return 10;
}
*/

/*************************************************************************/
static int getMaxBitDepthFromSPSPBitDepth(AL_THevcSps const* pSPS)
{
  int iSPSLumaBitDepth = pSPS->bit_depth_luma_minus8 + 8;
  int iSPSChromaBitDepth = pSPS->bit_depth_chroma_minus8 + 8;
  int iMaxSPSBitDepth = Max(iSPSLumaBitDepth, iSPSChromaBitDepth);
  int iMaxBitDepth = iMaxSPSBitDepth;

  if((iMaxBitDepth % 2) != 0)
    iMaxBitDepth++;
  return iMaxBitDepth;
}

/*****************************************************************************/
static int calculatePOC(AL_TPictMngrCtx* pCtx, AL_THevcSliceHdr* pSlice, uint8_t uNoRasOutputFlag)
{
  int32_t POCMsb = 0;
  int32_t MaxPOCLsb = pSlice->pSPS->MaxPicOrderCntLsb;
  int32_t MaxPOCLsb_div2 = MaxPOCLsb >> 1;

  if((!AL_HEVC_IsBLA(pSlice->nal_unit_type) &&
      !AL_HEVC_IsCRA(pSlice->nal_unit_type) &&
      !AL_HEVC_IsIDR(pSlice->nal_unit_type)) || !uNoRasOutputFlag)
  {
    if((pSlice->slice_pic_order_cnt_lsb < pCtx->iPrevPocLSB) &&
       ((pCtx->iPrevPocLSB - pSlice->slice_pic_order_cnt_lsb) >= MaxPOCLsb_div2))
      POCMsb = pCtx->iPrevPocMSB + MaxPOCLsb;

    else if((pSlice->slice_pic_order_cnt_lsb > pCtx->iPrevPocLSB) &&
            ((pSlice->slice_pic_order_cnt_lsb - pCtx->iPrevPocLSB) > MaxPOCLsb_div2))
      POCMsb = pCtx->iPrevPocMSB - MaxPOCLsb;

    else
      POCMsb = pCtx->iPrevPocMSB;
  }

  if(!(pSlice->nuh_temporal_id_plus1 - 1) && !AL_HEVC_IsRASL_RADL_SLNR(pSlice->nal_unit_type))
  {
    pCtx->iPrevPocLSB = pSlice->slice_pic_order_cnt_lsb;
    pCtx->iPrevPocMSB = POCMsb;
  }

  return pSlice->slice_pic_order_cnt_lsb + POCMsb;
}

/*****************************************************************************/
static AL_ESequenceMode GetSequenceModeFromScanType(uint8_t source_scan_type)
{
  switch(source_scan_type)
  {
  case 0: return AL_SM_INTERLACED;
  case 1: return AL_SM_PROGRESSIVE;

  case 2:
  case 3: return AL_SM_UNKNOWN;

  default: return AL_SM_MAX_ENUM;
  }
}

/*****************************************************************************/
static AL_ESequenceMode getSequenceMode(AL_THevcSps const* pSPS)
{
  AL_THevcProfilevel const* pProfileLevel = &pSPS->profile_and_level;

  if(pSPS->vui_parameters_present_flag)
  {
    AL_TVuiParam const* pVUI = &pSPS->vui_param;

    if(pVUI->field_seq_flag)
      return AL_SM_INTERLACED;
  }

  if(pSPS->profile_and_level.general_frame_only_constraint_flag)
    return AL_SM_PROGRESSIVE;

  if((pProfileLevel->general_progressive_source_flag == 0) && (pProfileLevel->general_interlaced_source_flag == 0))
    return AL_SM_UNKNOWN;

  if(pProfileLevel->general_progressive_source_flag && (pProfileLevel->general_interlaced_source_flag == 0))
    return AL_SM_PROGRESSIVE;

  if((pProfileLevel->general_progressive_source_flag == 0) && pProfileLevel->general_interlaced_source_flag)
    return AL_SM_INTERLACED;

  if(pProfileLevel->general_progressive_source_flag && pProfileLevel->general_interlaced_source_flag)
    return GetSequenceModeFromScanType(pSPS->sei_source_scan_type);

  return AL_SM_MAX_ENUM;
}

/******************************************************************************/
static void extractStreamSettings(AL_THevcSps const* pSPS, AL_TStreamSettings* pStreamSettings)
{
  AL_TDimension tSPSDim = { pSPS->pic_width_in_luma_samples, pSPS->pic_height_in_luma_samples };
  uint32_t uFlags = (pSPS->profile_and_level.general_max_12bit_constraint_flag << 15) |
                    (pSPS->profile_and_level.general_max_10bit_constraint_flag << 14) |
                    (pSPS->profile_and_level.general_max_8bit_constraint_flag << 13) |
                    (pSPS->profile_and_level.general_max_422chroma_constraint_flag << 12) |
                    (pSPS->profile_and_level.general_max_420chroma_constraint_flag << 11) |
                    (pSPS->profile_and_level.general_max_monochrome_constraint_flag << 10) |
                    (pSPS->profile_and_level.general_intra_constraint_flag << 9) |
                    (pSPS->profile_and_level.general_one_picture_only_constraint_flag << 8) |
                    (pSPS->profile_and_level.general_lower_bit_rate_constraint_flag << 7) |
                    (pSPS->profile_and_level.general_max_14bit_constraint_flag << 6);

  pStreamSettings->tDim = tSPSDim;
  pStreamSettings->eChroma = (AL_EChromaMode)pSPS->chroma_format_idc;
  pStreamSettings->iBitDepth = getMaxBitDepthFromSPSPBitDepth(pSPS);
  pStreamSettings->iLevel = pSPS->profile_and_level.general_level_idc / 3;
  pStreamSettings->eProfile = AL_PROFILE_HEVC | pSPS->profile_and_level.general_profile_idc | AL_RExt_FLAGS(uFlags);
  pStreamSettings->eSequenceMode = getSequenceMode(pSPS);
  pStreamSettings->iMaxRef = 0;
}

/*****************************************************************************/
static bool isStillPictureProfileSPS(AL_THevcSps const* pSPS)
{
  return (pSPS->profile_and_level.general_profile_idc == HEVC_PROFILE_IDC_MAIN_STILL) ||
         (pSPS->profile_and_level.general_profile_idc == HEVC_PROFILE_IDC_MAIN10 && pSPS->profile_and_level.general_one_picture_only_constraint_flag) ||
         (pSPS->profile_and_level.general_profile_idc >= HEVC_PROFILE_IDC_RExt && pSPS->profile_and_level.general_one_picture_only_constraint_flag);
}

static bool isIntraProfileSPS(AL_THevcSps const* pSPS)
{
  return isStillPictureProfileSPS(pSPS) ||
         (pSPS->profile_and_level.general_profile_idc >= 4 && pSPS->profile_and_level.general_intra_constraint_flag);
}

/*****************************************************************************/
static AL_ERR resolutionFound(AL_TDecCtx* pCtx, AL_TStreamSettings const* pCurrentStreamSettings, AL_TCropInfo const* pCropInfo)
{
  int iMaxBuf = AL_HEVC_GetMinOutputBuffersNeeded(pCurrentStreamSettings, pCtx->iStackSize);
  return pCtx->tDecCB.resolutionFoundCB.func(iMaxBuf, pCurrentStreamSettings, pCropInfo, pCtx->tDecCB.resolutionFoundCB.userParam);
}

/*****************************************************************************/
static AL_ERR isSPSCompatibleWithHardware(AL_THevcSps const* pSPS)
{
  int iSPSMaxBitDepth = getMaxBitDepthFromSPSPBitDepth(pSPS);

  if(iSPSMaxBitDepth > AL_HWConfig_Dec_GetSupportedBitDepth())
  {
    VIDEO_ERROR_PRINT("Bitdepth '%i' is not supported by the HARDWARE. Maximum supported bitdepth is '%i'.", iSPSMaxBitDepth, AL_HWConfig_Dec_GetSupportedBitDepth());
    return AL_WARN_SPS_BITDEPTH_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  AL_TDimension tSPSDim = { pSPS->pic_width_in_luma_samples, pSPS->pic_height_in_luma_samples };

  int32_t iMinResolution = ((AL_CORE_MIN_CU_NB - 1) << pSPS->Log2CtbSize) + (1 << pSPS->Log2MinCbSize);

  if(tSPSDim.iWidth < iMinResolution)
  {
    VIDEO_ERROR_PRINT("Width '%i' is not supported by the HARDWARE. Minimum supported width is '%i'.", tSPSDim.iWidth, iMinResolution);
    return AL_WARN_SPS_MIN_RESOLUTION_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  if(tSPSDim.iHeight < iMinResolution)
  {
    VIDEO_ERROR_PRINT("Height '%i' is not supported by the HARDWARE. Minimum supported height is '%i'.", tSPSDim.iHeight, iMinResolution);
    return AL_WARN_SPS_MIN_RESOLUTION_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  AL_EChromaMode eSPSChromaMode = (AL_EChromaMode)pSPS->chroma_format_idc;

  if(eSPSChromaMode > AL_HWConfig_Dec_GetSupportedChromaMode())
  {
    VIDEO_ERROR_PRINT("Chromamode '%i' is not supported by the HARDWARE. Maximum chromamode supported is '%i'.", eSPSChromaMode, AL_HWConfig_Dec_GetSupportedChromaMode());
    return AL_WARN_SPS_CHROMA_MODE_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  AL_ESequenceMode sequenceMode = getSequenceMode(pSPS);

  if(sequenceMode >= AL_SM_MAX_ENUM)
    return AL_WARN_SPS_INTERLACE_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;

  return AL_SUCCESS;
}

/*****************************************************************************/
static AL_ERR isSPSCompatibleWithInitialStreamSettings(AL_TDecCtx const* pCtx, AL_THevcSps const* pSPS)
{
  AL_TStreamSettings const* pStreamSettings = &pCtx->tInitialStreamSettings;

  if(!CheckStreamSettings(pStreamSettings))
    return AL_ERR_REQUEST_MALFORMED;

  int iSPSLumaBitDepth = pSPS->bit_depth_luma_minus8 + 8;

  if(pStreamSettings->iBitDepth < iSPSLumaBitDepth)
  {
    VIDEO_ERROR_PRINT("Bitdepth luma '%i' is not supported by the CHANNEL. Maximum supported bitdepth is '%i'.", iSPSLumaBitDepth, pStreamSettings->iBitDepth);
    return AL_WARN_SPS_BITDEPTH_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  int iSPSChromaBitDepth = pSPS->bit_depth_chroma_minus8 + 8;

  if(pStreamSettings->iBitDepth < iSPSChromaBitDepth)
  {
    VIDEO_ERROR_PRINT("Bitdepth chroma '%i' is not supported by the CHANNEL. Maximum supported bitdepth is '%i'.", iSPSChromaBitDepth, pStreamSettings->iBitDepth);
    return AL_WARN_SPS_BITDEPTH_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  AL_TDimension tSPSDim = { pSPS->pic_width_in_luma_samples, pSPS->pic_height_in_luma_samples };

  int iSPSLevel = pSPS->profile_and_level.general_level_idc / 3;
  int iCurDPBSize = Max(AL_HEVC_GetMaxDPBSize(pStreamSettings->iLevel, pStreamSettings->tDim.iWidth, pStreamSettings->tDim.iHeight, false, false, false), pStreamSettings->iMaxRef);
  int iNewDPBSize = Min(AL_HEVC_GetMaxDPBSize(iSPSLevel, tSPSDim.iWidth, tSPSDim.iHeight, false, false, false), pSPS->sps_max_dec_pic_buffering_minus1[0] + 1);

  if(iNewDPBSize > iCurDPBSize)
    return AL_WARN_SPS_LEVEL_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;

  AL_EChromaMode eSPSChromaMode = (AL_EChromaMode)pSPS->chroma_format_idc;

  if(pStreamSettings->eChroma < eSPSChromaMode)
  {
    VIDEO_ERROR_PRINT("Chroma mode '%i' is not supported by the CHANNEL. Maximum chromamode supported is '%i'.", eSPSChromaMode, pStreamSettings->eChroma);
    return AL_WARN_SPS_CHROMA_MODE_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  if(pStreamSettings->tDim.iWidth < tSPSDim.iWidth)
  {
    VIDEO_ERROR_PRINT("Width '%i' is not supported by the CHANNEL. Maximum supported width is '%i'.", tSPSDim.iWidth, pStreamSettings->tDim.iWidth);
    return AL_WARN_SPS_RESOLUTION_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  if(pStreamSettings->tDim.iHeight < tSPSDim.iHeight)
  {
    VIDEO_ERROR_PRINT("Height '%i' is not supported by the CHANNEL. Maximum supported height is '%i'.", tSPSDim.iHeight, pStreamSettings->tDim.iHeight);
    return AL_WARN_SPS_RESOLUTION_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  AL_ESequenceMode sequenceMode = getSequenceMode(pSPS);

  if((sequenceMode != AL_SM_UNKNOWN) && (pStreamSettings->eSequenceMode != AL_SM_UNKNOWN) && (pStreamSettings->eSequenceMode != sequenceMode))
  {
    VIDEO_ERROR_PRINT("Sequence '%i' is not supported by the CHANNEL. Current sequence mode is '%i'.", sequenceMode, pStreamSettings->eSequenceMode);
    return AL_WARN_SPS_INTERLACE_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  return AL_SUCCESS;
}

/*****************************************************************************/
static bool allocateBuffers(AL_TDecCtx* pCtx, AL_THevcSps const* pSPS)
{
  AL_TStreamSettings const* pStreamSettings = &pCtx->tCurrentStreamSettings;
  int iSPSMaxSlices = AL_HEVC_GetMaxNumberOfSlices(pStreamSettings->iLevel);
  int iSizeWP = iSPSMaxSlices * WP_SLICE_SIZE;
  int iSizeSP = iSPSMaxSlices * sizeof(AL_TDecSliceParam);
  int iSizeCompData = AL_GetAllocSize_HevcCompData(pStreamSettings->tDim, pStreamSettings->eChroma);
  int iSizeCompMap = AL_GetAllocSize_DecCompMap(pStreamSettings->tDim);
  AL_ERR error = AL_ERR_NO_MEMORY;

  if(!AL_Default_Decoder_AllocPool(pCtx, 0, 0, iSizeWP, iSizeSP, iSizeCompData, iSizeCompMap, 0))
    goto fail_alloc;

  int const iDpbMaxBuf = AL_HEVC_GetMaxDpbBuffers(pStreamSettings);
  int iMaxBuf = AL_HEVC_GetMinOutputBuffersNeeded(pStreamSettings, pCtx->iStackSize);
  int iSizeMV = AL_GetAllocSize_HevcMV(pStreamSettings->tDim);
  int iSizePOC = POCBUFF_PL_SIZE;

  if(!AL_Default_Decoder_AllocMv(pCtx, iSizeMV, iSizePOC, iMaxBuf))
    goto fail_alloc;

  int iDpbRef = Min(pSPS->sps_max_dec_pic_buffering_minus1[pSPS->sps_max_sub_layers_minus1] + 1, iDpbMaxBuf);

  if(isIntraProfileSPS(pSPS))
    iDpbRef = 1;

  AL_TPictMngrParam tPictMngrParam;
  tPictMngrParam.iNumDPBRef = iDpbRef;
  tPictMngrParam.eDPBMode = pCtx->eDpbMode;
  tPictMngrParam.eFbStorageMode = pCtx->pChanParam->eFBStorageMode;
  tPictMngrParam.iNumMV = iMaxBuf;
  tPictMngrParam.iSizeMV = iSizeMV;
  tPictMngrParam.bForceOutput = pCtx->pChanParam->bUseEarlyCallback;
  tPictMngrParam.tOutputPosition = pCtx->tOutputPosition;

  if(!AL_PictMngr_BasicInit(&pCtx->PictMngr, &tPictMngrParam))
    goto fail_alloc;

  AL_TCropInfo tCropInfo;
  AL_HEVC_GetCropInfo(pSPS, &tCropInfo);
  error = resolutionFound(pCtx, pStreamSettings, &tCropInfo);

  if(AL_IS_ERROR_CODE(error))
    goto fail_alloc;

  Rtos_WaitEvent(pCtx->hDecOutSettingsConfiguredEvt, AL_WAIT_FOREVER);

  return true;

  fail_alloc:
  AL_Default_Decoder_SetError(pCtx, error, -1, true);
  return false;
}

/******************************************************************************/
static bool initChannel(AL_TDecCtx* pCtx, AL_THevcSps const* pSPS)
{
  AL_TDecChanParam* pChan = pCtx->pChanParam;
  AL_TStreamSettings const* pStreamSettings = &pCtx->tCurrentStreamSettings;
  pChan->iWidth = pStreamSettings->tDim.iWidth;
  pChan->iHeight = pStreamSettings->tDim.iHeight;
  pChan->uLog2MaxCuSize = pSPS->Log2CtbSize;
  pChan->eMaxChromaMode = pStreamSettings->eChroma;

  const int iSPSMaxSlices = AL_HEVC_GetMaxNumberOfSlices(pStreamSettings->iLevel);
  pChan->iMaxSlices = iSPSMaxSlices;
  const int iSPSMaxTileRows = AL_HEVC_GetMaxTileRows(pStreamSettings->iLevel);
  const int iSPSMaxTileCols = AL_HEVC_GetMaxTileColumns(pStreamSettings->iLevel);
  const int iPicMaxTileRows = RoundUp(pChan->iHeight, 64) / 64;
  const int iPicMaxTileCols = RoundUp(pChan->iWidth, 256) / 256;
  pChan->iMaxTiles = Min(iSPSMaxTileCols, iPicMaxTileCols) * Min(iSPSMaxTileRows, iPicMaxTileRows);

  if(!pCtx->bForceFrameRate && pSPS->vui_parameters_present_flag && pSPS->vui_param.vui_timing_info_present_flag)
  {
    pChan->uFrameRate = pSPS->vui_param.vui_time_scale;
    pChan->uClkRatio = pSPS->vui_param.vui_num_units_in_tick;

    if(pCtx->uConcealMaxFps && pChan->uFrameRate / pChan->uClkRatio > pCtx->uConcealMaxFps)
    {
      pChan->uFrameRate = pCtx->uConcealMaxFps;
      pChan->uClkRatio = 1;
    }
  }

  return AL_Default_Decoder_CreateChannel(pCtx, AL_Default_Decoder_EndParsing, AL_Default_Decoder_EndDecoding);
}

/******************************************************************************/
static int slicePpsId(AL_THevcSliceHdr const* pSlice)
{
  return pSlice->slice_pic_parameter_set_id;
}

/******************************************************************************/
static int sliceSpsId(AL_THevcPps const* pPps, AL_THevcSliceHdr const* pSlice)
{
  int const ppsid = slicePpsId(pSlice);
  return pPps[ppsid].pps_seq_parameter_set_id;
}

/*****************************************************************************/
static bool initSlice(AL_TDecCtx* pCtx, AL_THevcSliceHdr* pSlice)
{
  AL_TAup* pIAup = &pCtx->aup;
  AL_THevcAup* pAup = &pIAup->hevcAup;

  if(!pCtx->bIsFirstSPSChecked)
  {
    AL_ERR const ret = !pCtx->bAreBuffersAllocated ? isSPSCompatibleWithHardware(pSlice->pSPS) : isSPSCompatibleWithInitialStreamSettings(pCtx, pSlice->pSPS);

    if(ret != AL_SUCCESS)
    {
      VIDEO_ERROR_PRINT("Cannot decode using the current allocated buffers.");
      pSlice->pPPS = &pAup->pPPS[pCtx->tConceal.iLastPPSId];
      pSlice->pSPS = pSlice->pPPS->pSPS;
      AL_Default_Decoder_SetError(pCtx, ret, -1, true);
      return false;
    }

    if(!pCtx->bAreBuffersAllocated)
    {
      extractStreamSettings(pSlice->pSPS, &pCtx->tCurrentStreamSettings);
      pCtx->tInitialStreamSettings = pCtx->tCurrentStreamSettings;
    }

    pCtx->bIsFirstSPSChecked = true;
    pCtx->bIntraOnlyProfile = isIntraProfileSPS(pSlice->pSPS);
    pCtx->bStillPictureProfile = isStillPictureProfileSPS(pSlice->pSPS);

    if(!pCtx->bAreBuffersAllocated)
      if(!allocateBuffers(pCtx, pSlice->pSPS))
        return false;

    if(!initChannel(pCtx, pSlice->pSPS))
      return false;

    pCtx->bAreBuffersAllocated = true;
  }

  int const spsid = sliceSpsId(pAup->pPPS, pSlice);

  pAup->pActiveSPS = &pAup->pSPS[spsid];

  const AL_TDimension tDim = { pSlice->pSPS->pic_width_in_luma_samples, pSlice->pSPS->pic_height_in_luma_samples };
  const int iLevel = pSlice->pSPS->profile_and_level.general_level_idc / 3;
  const int iDpbMaxBuf = AL_HEVC_GetMaxDPBSize(iLevel, tDim.iWidth, tDim.iHeight, false, false, false);
  const int iDpbRef = Min(pSlice->pSPS->sps_max_dec_pic_buffering_minus1[pSlice->pSPS->sps_max_sub_layers_minus1] + 1, iDpbMaxBuf);
  AL_PictMngr_UpdateDPBInfo(&pCtx->PictMngr, iDpbRef);

  if(!pSlice->dependent_slice_segment_flag)
  {
    if(pSlice->slice_type != AL_SLICE_I && (pIAup->iRecoveryCnt == 0) && !AL_HEVC_PictMngr_HasPictInDPB(&pCtx->PictMngr))
      return false;

    if(pSlice->IdrPicFlag)
    {
      pCtx->PictMngr.iCurFramePOC = 0;
      pCtx->PictMngr.iPrevPocLSB = 0;
      pCtx->PictMngr.iPrevPocMSB = 0;
    }
    else if(!pCtx->tConceal.bValidFrame)
      pCtx->PictMngr.iCurFramePOC = calculatePOC(&pCtx->PictMngr, pSlice, pCtx->uNoRaslOutputFlag);

    if(!pCtx->tConceal.bValidFrame)
    {
      AL_HEVC_PictMngr_InitRefPictSet(&pCtx->PictMngr, pSlice);

      /* at least one active reference on inter slice */
      if(pSlice->slice_type != AL_SLICE_I && !pSlice->NumPocTotalCurr && !AL_HEVC_PictMngr_HasPictInDPB(&pCtx->PictMngr))
        return false;
    }
  }

  return true;
}

/*****************************************************************************/
static void copyScalingList(AL_THevcPps* pPPS, AL_TScl* pSCL)
{
  Rtos_Memcpy((*pSCL)[0].t4x4Y, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[0] ? AL_HEVC_DefaultScalingLists4x4[0] :
              pPPS->scaling_list_param.ScalingList[0][0], 16);

  Rtos_Memcpy((*pSCL)[0].t4x4Cb, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[1] ? AL_HEVC_DefaultScalingLists4x4[0] :
              pPPS->scaling_list_param.ScalingList[0][1], 16);

  Rtos_Memcpy((*pSCL)[0].t4x4Cr, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[2] ? AL_HEVC_DefaultScalingLists4x4[0] :
              pPPS->scaling_list_param.ScalingList[0][2], 16);

  Rtos_Memcpy((*pSCL)[1].t4x4Y, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[3] ? AL_HEVC_DefaultScalingLists4x4[1] :
              pPPS->scaling_list_param.ScalingList[0][3], 16);

  Rtos_Memcpy((*pSCL)[1].t4x4Cb, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[4] ? AL_HEVC_DefaultScalingLists4x4[1] :
              pPPS->scaling_list_param.ScalingList[0][4], 16);

  Rtos_Memcpy((*pSCL)[1].t4x4Cr, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[5] ? AL_HEVC_DefaultScalingLists4x4[1] :
              pPPS->scaling_list_param.ScalingList[0][5], 16);

  Rtos_Memcpy((*pSCL)[0].t8x8Y, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[6] ? AL_HEVC_DefaultScalingLists8x8[0] :
              pPPS->scaling_list_param.ScalingList[1][0], 64);

  Rtos_Memcpy((*pSCL)[0].t8x8Cb, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[7] ? AL_HEVC_DefaultScalingLists8x8[0] :
              pPPS->scaling_list_param.ScalingList[1][1], 64);

  Rtos_Memcpy((*pSCL)[0].t8x8Cr, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[8] ? AL_HEVC_DefaultScalingLists8x8[0] :
              pPPS->scaling_list_param.ScalingList[1][2], 64);

  Rtos_Memcpy((*pSCL)[1].t8x8Y, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[9] ? AL_HEVC_DefaultScalingLists8x8[1] :
              pPPS->scaling_list_param.ScalingList[1][3], 64);

  Rtos_Memcpy((*pSCL)[1].t8x8Cb, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[10] ? AL_HEVC_DefaultScalingLists8x8[1] :
              pPPS->scaling_list_param.ScalingList[1][4], 64);

  Rtos_Memcpy((*pSCL)[1].t8x8Cr, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[11] ? AL_HEVC_DefaultScalingLists8x8[1] :
              pPPS->scaling_list_param.ScalingList[1][5], 64);

  Rtos_Memcpy((*pSCL)[0].t16x16Y, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[12] ? AL_HEVC_DefaultScalingLists8x8[0] :
              pPPS->scaling_list_param.ScalingList[2][0], 64);

  Rtos_Memcpy((*pSCL)[0].t16x16Cb, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[13] ? AL_HEVC_DefaultScalingLists8x8[0] :
              pPPS->scaling_list_param.ScalingList[2][1], 64);

  Rtos_Memcpy((*pSCL)[0].t16x16Cr, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[14] ? AL_HEVC_DefaultScalingLists8x8[0] :
              pPPS->scaling_list_param.ScalingList[2][2], 64);

  Rtos_Memcpy((*pSCL)[1].t16x16Y, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[15] ? AL_HEVC_DefaultScalingLists8x8[1] :
              pPPS->scaling_list_param.ScalingList[2][3], 64);

  Rtos_Memcpy((*pSCL)[1].t16x16Cb, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[16] ? AL_HEVC_DefaultScalingLists8x8[1] :
              pPPS->scaling_list_param.ScalingList[2][4], 64);

  Rtos_Memcpy((*pSCL)[1].t16x16Cr, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[17] ? AL_HEVC_DefaultScalingLists8x8[1] :
              pPPS->scaling_list_param.ScalingList[2][5], 64);

  Rtos_Memcpy((*pSCL)[0].t32x32, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[18] ? AL_HEVC_DefaultScalingLists8x8[0] :
              pPPS->scaling_list_param.ScalingList[3][0], 64);

  Rtos_Memcpy((*pSCL)[1].t32x32, pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[19] ? AL_HEVC_DefaultScalingLists8x8[1] :
              pPPS->scaling_list_param.ScalingList[3][3], 64);

  (*pSCL)[0].tDC[0] = pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[12] ? 16 : pPPS->scaling_list_param.scaling_list_dc_coeff[0][0];
  (*pSCL)[0].tDC[1] = pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[13] ? 16 : pPPS->scaling_list_param.scaling_list_dc_coeff[0][1];
  (*pSCL)[0].tDC[2] = pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[14] ? 16 : pPPS->scaling_list_param.scaling_list_dc_coeff[0][2];
  (*pSCL)[0].tDC[3] = pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[18] ? 16 : pPPS->scaling_list_param.scaling_list_dc_coeff[1][0];
  (*pSCL)[1].tDC[0] = pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[15] ? 16 : pPPS->scaling_list_param.scaling_list_dc_coeff[0][3];
  (*pSCL)[1].tDC[1] = pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[16] ? 16 : pPPS->scaling_list_param.scaling_list_dc_coeff[0][4];
  (*pSCL)[1].tDC[2] = pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[17] ? 16 : pPPS->scaling_list_param.scaling_list_dc_coeff[0][5];
  (*pSCL)[1].tDC[3] = pPPS->scaling_list_param.UseDefaultScalingMatrixFlag[19] ? 16 : pPPS->scaling_list_param.scaling_list_dc_coeff[1][3];
}

/*****************************************************************************/
static bool isFirstSliceSegmentInPicture(AL_THevcSliceHdr* pSlice)
{
  return pSlice->first_slice_segment_in_pic_flag == 1;
}

/*****************************************************************************/
static void processScalingList(AL_THevcAup* pAUP, AL_THevcSliceHdr* pSlice, AL_TScl* pScl)
{
  int const ppsid = slicePpsId(pSlice);

  AL_CleanupMemory(pScl, sizeof(*pScl));

  // Save ScalingList
  if(pAUP->pPPS[ppsid].pSPS->scaling_list_enabled_flag && isFirstSliceSegmentInPicture(pSlice))
    copyScalingList(&pAUP->pPPS[ppsid], pScl);
}

/*****************************************************************************/
extern void AL_HEVC_SetDefaultSliceHeader(AL_THevcSliceHdr* pSlice);

/*****************************************************************************/
static void concealSlice(AL_TDecCtx* pCtx, AL_TDecPicParam* pPP, AL_TDecSliceParam* pSP, AL_THevcSliceHdr* pSlice, bool bSliceHdrValid)
{
  if(!bSliceHdrValid)
    AL_HEVC_SetDefaultSliceHeader(pSlice);

  pSlice->slice_type = AL_SLICE_CONCEAL;
  AL_Default_Decoder_SetError(pCtx, AL_WARN_CONCEAL_DETECT, pPP->tBufIDs.FrmID, true);

  AL_HEVC_FillPictParameters(pSlice, pCtx, pPP);
  AL_HEVC_FillSliceParameters(pSlice, pCtx, pSP);

  AL_SetConcealParameters(pCtx, pSP);

  if(bSliceHdrValid)
    pSP->FirstLcuSliceSegment = pSP->FirstLcuSlice = pSP->SliceFirstLCU = pSlice->slice_segment_address;

  AL_SET_DEC_OPT(pPP, IntraOnly, 0);
}

/*****************************************************************************/
static void createConcealSlice(AL_TDecCtx* pCtx, AL_TDecPicParam* pPP, AL_TDecSliceParam* pSP, AL_THevcSliceHdr* pSlice)
{
  uint8_t uCurSliceType = pSlice->slice_type;

  concealSlice(pCtx, pPP, pSP, pSlice, false);
  pSP->FirstLcuSliceSegment = 0;
  pSP->FirstLcuSlice = 0;
  pSP->SliceFirstLCU = 0;
  pSP->NextSliceSegment = pSlice->slice_segment_address;
  pSP->SliceNumLCU = pSlice->slice_segment_address;

  pSlice->slice_type = uCurSliceType;
}

/*****************************************************************************/
static void reallyEndFrame(AL_TDecCtx* pCtx, AL_ENut eNUT, AL_THevcSliceHdr* pSlice, uint8_t pic_output_flag, bool bHasPreviousSlice)
{
  AL_HEVC_PictMngr_EndFrame(&pCtx->PictMngr, pSlice->slice_pic_order_cnt_lsb, eNUT, pSlice, pic_output_flag);

  if(pCtx->pChanParam->eDecUnit == AL_AU_UNIT)
    AL_LaunchFrameDecoding(pCtx);

  if(pCtx->pChanParam->eDecUnit == AL_VCL_NAL_UNIT)
    AL_LaunchSliceDecoding(pCtx, true, bHasPreviousSlice);

  UpdateContextAtEndOfFrame(pCtx);
}

static void endFrame(AL_TDecCtx* pCtx, AL_ENut eNUT, AL_THevcSliceHdr* pSlice, uint8_t pic_output_flag)
{
  reallyEndFrame(pCtx, eNUT, pSlice, pic_output_flag, true);
}

static void endFrameConceal(AL_TDecCtx* pCtx, AL_ENut eNUT, AL_THevcSliceHdr* pSlice, uint8_t pic_output_flag)
{
  reallyEndFrame(pCtx, eNUT, pSlice, pic_output_flag, false);
}

/*****************************************************************************/
static void finishPreviousFrame(AL_TDecCtx* pCtx)
{
  AL_THevcSliceHdr* pSlice = &pCtx->HevcSliceHdr[pCtx->uCurID];
  AL_TDecPicParam* pPP = &pCtx->PoolPP[pCtx->uToggle];
  AL_TDecSliceParam* pSP = &(((AL_TDecSliceParam*)pCtx->PoolSP[pCtx->uToggle].tMD.pVirtualAddr)[pCtx->tCurrentFrameCtx.uNumSlice - 1]);
  AL_TDecPicBuffers* pBufs = &pCtx->PoolPB[pCtx->uToggle];

  AL_TerminatePreviousCommand(pCtx, pPP, pSP, pBufs, true, false);

  // copy stream offset from previous command
  pCtx->iStreamOffset[pCtx->iNumFrmBlk1 % pCtx->iStackSize] = pCtx->iStreamOffset[(pCtx->iNumFrmBlk1 + pCtx->iStackSize - 1) % pCtx->iStackSize];

  /* The slice is its own previous slice as we changed it in the last slice
   * This means that in AL_VCL_NAL_UNIT we don't want to send a previous slice at all. */
  endFrameConceal(pCtx, pSlice->nal_unit_type, pSlice, pSlice->pic_output_flag);

  AL_DecFrameCtx_Reset(&pCtx->tCurrentFrameCtx);
}

/*****************************************************************************/
static bool isRandomAccessPoint(AL_ENut eNUT)
{
  return AL_HEVC_IsCRA(eNUT) || AL_HEVC_IsBLA(eNUT) || AL_HEVC_IsIDR(eNUT) || (eNUT == AL_HEVC_NUT_RSV_IRAP_VCL22) || (eNUT == AL_HEVC_NUT_RSV_IRAP_VCL23);
}

/*****************************************************************************/
static bool isValidSyncPoint(AL_TDecCtx* pCtx, AL_ENut eNUT, AL_ESliceType ePicType, int32_t iRecoveryCnt)
{
  if(isRandomAccessPoint(eNUT))
    return true;

  if(iRecoveryCnt != 0)
    return true;

  if(pCtx->bUseIFramesAsSyncPoint && eNUT == AL_HEVC_NUT_TRAIL_R && ePicType == AL_SLICE_I)
    return true;

  return false;
}

/*****************************************************************************/
static bool hevcInitFrameBuffers(AL_TDecCtx* pCtx, bool bStartsNewCVS, const AL_THevcSps* pSPS, AL_TDecPicParam* pPP, AL_TDecPicBuffers* pBufs)
{
  (void)bStartsNewCVS;
  AL_TDimension const tDim = { pSPS->pic_width_in_luma_samples, pSPS->pic_height_in_luma_samples };

  if(!AL_InitFrameBuffers(pCtx, pBufs, bStartsNewCVS, tDim, (AL_EChromaMode)pSPS->chroma_format_idc, pPP))
    return false;

  AL_TBuffer* pDispBuf = AL_PictMngr_GetDisplayBufferFromID(&pCtx->PictMngr, pPP->tBufIDs.FrmID);
  AL_THDRMetaData* pMeta = (AL_THDRMetaData*)AL_Buffer_GetMetaData(pDispBuf, AL_META_TYPE_HDR);

  if(pMeta != NULL)
  {
    AL_THDRSEIs* pHDRSEIs = &pCtx->aup.tParsedHDRSEIs;
    pMeta->eColourDescription = AL_H273_ColourPrimariesToColourDesc(pSPS->vui_param.colour_primaries);
    pMeta->eTransferCharacteristics = AL_VUIValueToTransferCharacteristics(pSPS->vui_param.transfer_characteristics);
    pMeta->eColourMatrixCoeffs = AL_VUIValueToColourMatrixCoefficients(pSPS->vui_param.matrix_coefficients);
    AL_HDRSEIs_Copy(pHDRSEIs, &pMeta->tHDRSEIs);
    AL_HDRSEIs_Reset(pHDRSEIs);
  }

  return true;
}

/*****************************************************************************/
static void hevcGetCropInfo(AL_TDecCtx* pCtx, AL_THevcSps const* pSPS, AL_TCropInfo* pCropInfo)
{
  (void)pCtx;

  AL_HEVC_GetCropInfo(pSPS, pCropInfo);
}

/*****************************************************************************/
static bool decodeSliceData(AL_TAup* pIAUP, AL_TDecCtx* pCtx, AL_ENut eNUT, bool bIsLastAUNal, int* iNumSlice)
{
  AL_TDecFrameCtx* pFrmCtx = &pCtx->tCurrentFrameCtx;

  if(pFrmCtx->bFirstSliceValid && *iNumSlice > pCtx->pChanParam->iMaxSlices)
    return false;

  // ignore RASL picture associated with an IRAP picture that has NoRaslOutputFlag = 1
  if(AL_HEVC_IsRASL(eNUT) && pCtx->uNoRaslOutputFlag)
  {
    if(bIsLastAUNal)
    {
      if(pFrmCtx->eBufStatus & DEC_FRAME_BUF_RESERVED)
        AL_CancelFrameBuffers(pCtx);
      else
        UpdateContextAtEndOfFrame(pCtx);
    }
    return false;
  }

  bool const bIsRAP = isRandomAccessPoint(eNUT);

  if(bIsRAP)
    pCtx->uNoRaslOutputFlag = (pCtx->bIsFirstPicture || AL_HEVC_IsBLA(eNUT) || AL_HEVC_IsIDR(eNUT)) ? 1 : 0;

  AL_THevcAup* pAUP = &pIAUP->hevcAup;
  AL_TStreamSettings const* pStreamSettings = &pCtx->tCurrentStreamSettings;

  TCircBuffer* pBufStream = &pCtx->Stream;
  // Slice header deanti-emulation
  AL_TRbspParser rp;
  InitRbspParser(pBufStream, pCtx->BufNoAE.tMD.pVirtualAddr, pCtx->BufNoAE.tMD.uSize, true, &rp);

  // Parse Slice Header
  uint8_t uToggleID = (~pCtx->uCurID) & 0x01;
  AL_THevcSliceHdr* pSlice = &pCtx->HevcSliceHdr[uToggleID];
  Rtos_Memset(pSlice, 0, offsetof(AL_THevcSliceHdr, entry_point_offset_minus1));
  AL_TConceal* pConceal = &pCtx->tConceal;
  bool isSliceHdrValid = AL_HEVC_ParseSliceHeader(pSlice, &pCtx->HevcSliceHdr[pCtx->uCurID], &rp, pConceal, pAUP->pPPS);
  bool bSliceBelongsToSameFrame = true;

  if((!isSliceHdrValid || !pSlice->pPPS) && pConceal->iLastPPSId >= 0)
    pSlice->pPPS = &pAUP->pPPS[pConceal->iLastPPSId];

  if(!isSliceHdrValid || !pSlice->pSPS)
    pSlice->pSPS = pAUP->pActiveSPS;

  bool isValid = isSliceHdrValid;

  if(isValid)
  {
    if((pSlice->slice_pic_order_cnt_lsb != pCtx->iCurPocLsb) && !isFirstSliceSegmentInPicture(pSlice))
      bSliceBelongsToSameFrame = false;
    else if((pSlice->slice_segment_address <= pConceal->iFirstLCU) && !pSlice->pPPS->tiles_enabled_flag && !pSlice->pPPS->entropy_coding_sync_enabled_flag)
    {
      if(!(AL_HEVC_IsIDR(eNUT)))
        isValid = false;
      else
        bSliceBelongsToSameFrame = false;
    }
  }

  if(isValid)
    pConceal->iFirstLCU = pSlice->slice_segment_address;

  bool* bFirstIsValid = &pCtx->bFirstIsValid;

  if(!bSliceBelongsToSameFrame && AL_Default_Decoder_HasOngoingFrame(pCtx))
  {
    finishPreviousFrame(pCtx);

    if(pCtx->eInputMode == AL_DEC_SPLIT_INPUT)
    {
      pConceal->bSkipRemainingNals = true;
      return true;
    }
  }

  AL_TDecPicParam* pPP = &pCtx->PoolPP[pCtx->uToggle];

  if(isValid && pCtx->bAreBuffersAllocated)
  {
    AL_THevcSps* pSPS = pSlice->pSPS;
    AL_ERR const ret = isSPSCompatibleWithInitialStreamSettings(pCtx, pSPS);
    isValid = ret == AL_SUCCESS;
    AL_TStreamSettings spsSettings;

    if(isValid)
    {
      spsSettings.bDecodeIntraOnly = pCtx->tCurrentStreamSettings.bDecodeIntraOnly;
      extractStreamSettings(pSPS, &spsSettings);
      // get value from pre alloc
      spsSettings.iMaxRef = pCtx->tCurrentStreamSettings.iMaxRef;
    }

    if(!isValid)
    {
      VIDEO_ERROR_PRINT("SPS is not compatible with the current allocated buffers.");
      AL_Default_Decoder_SetError(pCtx, ret, pPP->tBufIDs.FrmID, true);
      pSPS->bConceal = true;
    }
    else if(spsSettings.tDim.iWidth != pStreamSettings->tDim.iWidth || spsSettings.tDim.iHeight != pStreamSettings->tDim.iHeight)
    {
      AL_TCropInfo tCropInfo;
      AL_HEVC_GetCropInfo(pSPS, &tCropInfo);
      AL_ERR error = resolutionFound(pCtx, &spsSettings, &tCropInfo);

      if(error != AL_SUCCESS)
      {
        VIDEO_ERROR_PRINT("Resolution change detected but resolution found callback returns with error.");
        AL_Default_Decoder_SetError(pCtx, AL_WARN_RES_FOUND_CB, pPP->tBufIDs.FrmID, false);
        pSPS->bConceal = true;
        isValid = false;
      }
      pCtx->tCurrentStreamSettings = spsSettings;
    }
  }

  if(isValid)
  {
    pCtx->iCurPocLsb = pSlice->slice_pic_order_cnt_lsb;
    isValid = initSlice(pCtx, pSlice);
  }

  if(!isValid)
  {
    if(!*bFirstIsValid)
    {
      if(bIsLastAUNal)
      {
        if(pFrmCtx->eBufStatus & DEC_FRAME_BUF_RESERVED)
          AL_CancelFrameBuffers(pCtx);
        else
          UpdateContextAtEndOfFrame(pCtx);
      }
      return false;
    }
    AL_HEVC_PictMngr_RemoveHeadFrame(&pCtx->PictMngr);
  }

  if(isValid && isFirstSliceSegmentInPicture(pSlice) && pFrmCtx->bFirstSliceValid)
    isValid = false;

  if(isValid && pSlice->slice_type != AL_SLICE_I)
    AL_SET_DEC_OPT(pPP, IntraOnly, 0);

  pCtx->uCurID = (pCtx->uCurID + 1) & 1;

  AL_TDecSliceParam* pSP = &(((AL_TDecSliceParam*)pCtx->PoolSP[pCtx->uToggle].tMD.pVirtualAddr)[pFrmCtx->uNumSlice]);

  AL_TDecPicBuffers* pBufs = &pCtx->PoolPB[pCtx->uToggle];
  pBufs->tStream.tMD = pCtx->Stream.tMD;

  if(isValid)
  {
    if(isFirstSliceSegmentInPicture(pSlice) && pFrmCtx->eBufStatus == DEC_FRAME_BUF_NONE)
    {
      bool bClearRef = (bIsRAP && pCtx->uNoRaslOutputFlag); // IRAP picture with NoRaslOutputFlag = 1
      bool bNoOutputPrior = (AL_HEVC_IsCRA(eNUT) || ((AL_HEVC_IsIDR(eNUT) || AL_HEVC_IsBLA(eNUT)) && pSlice->no_output_of_prior_pics_flag));

      AL_HEVC_PictMngr_ClearDPB(&pCtx->PictMngr, pSlice->pSPS, bClearRef, bNoOutputPrior);
    }

    if(pSlice->slice_type != AL_SLICE_I && (pIAUP->iRecoveryCnt == 0) && !AL_HEVC_PictMngr_HasPictInDPB(&pCtx->PictMngr))
      isValid = false;
    else if(isValid && !pFrmCtx->bFirstSliceValid && pSlice->slice_segment_address)
    {
      if(pSlice->slice_segment_address <= (int)pSP->NextSliceSegment)
      {
        createConcealSlice(pCtx, pPP, pSP, pSlice);

        pSP = &(((AL_TDecSliceParam*)pCtx->PoolSP[pCtx->uToggle].tMD.pVirtualAddr)[++pFrmCtx->uNumSlice]);
        pFrmCtx->bFirstSliceValid = true;
      }
      else
        isValid = false;
    }
  }

  if(pCtx->bAreBuffersAllocated && pFrmCtx->eBufStatus == DEC_FRAME_BUF_NONE && pSlice->pSPS)
  {
    if(!hevcInitFrameBuffers(pCtx, bIsRAP, pSlice->pSPS, pPP, pBufs))
      return false;

    AL_TCropInfo tCropInfo;
    hevcGetCropInfo(pCtx, pSlice->pSPS, &tCropInfo);
    pFrmCtx->eBufStatus = DEC_FRAME_BUF_RESERVED;
    AL_HEVC_PictMngr_UpdateRecInfo(&pCtx->PictMngr, &tCropInfo, pAUP->ePicStruct);
  }

  bool bLastSlice = *iNumSlice >= pCtx->pChanParam->iMaxSlices;

  if(bLastSlice && !bIsLastAUNal)
    isValid = false;

  AL_TScl ScalingList = { 0 };

  pFrmCtx->bIsIntraOnly &= pSlice->slice_type == AL_SLICE_I;

  if(pStreamSettings->bDecodeIntraOnly && !pFrmCtx->bIsIntraOnly && bIsLastAUNal)
    isValid = false;

  if(isValid)
  {
    if(!(*bFirstIsValid))
    {
      if(!isValidSyncPoint(pCtx, eNUT, pSlice->slice_type, pIAUP->iRecoveryCnt))
      {
        pFrmCtx->eBufStatus = DEC_FRAME_BUF_NONE;
        AL_CancelFrameBuffers(pCtx);
        return false;
      }
      *bFirstIsValid = true;
    }

    UpdateCircBuffer(&rp, pBufStream, &pSlice->slice_header_length);

    processScalingList(pAUP, pSlice, &ScalingList);

    if(pFrmCtx->uNumSlice == 0)
      AL_HEVC_FillPictParameters(pSlice, pCtx, pPP);
    AL_HEVC_FillSliceParameters(pSlice, pCtx, pSP);

    if(!AL_HEVC_PictMngr_BuildPictureList(&pCtx->PictMngr, pSlice, &pCtx->ListRef) && (pIAUP->iRecoveryCnt == 0))
    {
      concealSlice(pCtx, pPP, pSP, pSlice, isSliceHdrValid);
    }
    else
    {
      AL_HEVC_FillSlicePicIdRegister(pSlice, pCtx, pPP, pSP);
      pConceal->bValidFrame = true;
      AL_SetConcealParameters(pCtx, pSP);
    }
  }
  else if((bIsLastAUNal || isFirstSliceSegmentInPicture(pSlice) || bLastSlice) &&
          (*bFirstIsValid) && pFrmCtx->bFirstSliceValid &&
          !(pStreamSettings->bDecodeIntraOnly && !pFrmCtx->bIsIntraOnly)) /* conceal the current slice data */
  {
    concealSlice(pCtx, pPP, pSP, pSlice, isSliceHdrValid);

    if(bLastSlice)
      pSP->NextSliceSegment = pPP->LcuPicWidth * pPP->LcuPicHeight;
  }
  else // skip slice
  {
    if(bIsLastAUNal)
    {
      if(pFrmCtx->eBufStatus & DEC_FRAME_BUF_RESERVED)
      {
        AL_CancelFrameBuffers(pCtx);
        pFrmCtx->bIsIntraOnly = true;
      }
      else
        UpdateContextAtEndOfFrame(pCtx);
    }

    return false;
  }

  if(isValid && isFirstSliceSegmentInPicture(pSlice))
    pFrmCtx->bFirstSliceValid = true;

  // Launch slice decoding
  AL_HEVC_PrepareCommand(pCtx, &ScalingList, pPP, pBufs, pSP, pSlice, bIsLastAUNal || bLastSlice, isValid);

  ++pFrmCtx->uNumSlice;
  ++(*iNumSlice);

  if(bIsLastAUNal || bLastSlice)
  {
    uint8_t pic_output_flag = (AL_HEVC_IsRASL(eNUT) && pCtx->uNoRaslOutputFlag) ? 0 : pSlice->pic_output_flag;
    endFrame(pCtx, eNUT, pSlice, pic_output_flag);
    return true;
  }

  if(pCtx->pChanParam->eDecUnit == AL_VCL_NAL_UNIT)
  {
    AL_LaunchSliceDecoding(pCtx, false, true);
    return true;
  }

  return false;
}

/*****************************************************************************/
static bool isSliceData(AL_ENut nut)
{
  switch(nut)
  {
  case AL_HEVC_NUT_TRAIL_N:
  case AL_HEVC_NUT_TRAIL_R:
  case AL_HEVC_NUT_TSA_N:
  case AL_HEVC_NUT_TSA_R:
  case AL_HEVC_NUT_STSA_N:
  case AL_HEVC_NUT_STSA_R:
  case AL_HEVC_NUT_RADL_N:
  case AL_HEVC_NUT_RADL_R:
  case AL_HEVC_NUT_RASL_N:
  case AL_HEVC_NUT_RASL_R:
  case AL_HEVC_NUT_BLA_W_LP:
  case AL_HEVC_NUT_BLA_W_RADL:
  case AL_HEVC_NUT_BLA_N_LP:
  case AL_HEVC_NUT_IDR_W_RADL:
  case AL_HEVC_NUT_IDR_N_LP:
  case AL_HEVC_NUT_CRA:
    return true;
  default:
    return false;
  }
}

/*****************************************************************************/
static AL_PARSE_RESULT parsePPSandUpdateConcealment(AL_TAup* IAup, AL_TRbspParser* rp, AL_TDecCtx* pCtx)
{
  uint16_t PpsId;
  AL_PARSE_RESULT result = AL_HEVC_ParsePPS(IAup, rp, &PpsId);

  if(PpsId >= AL_HEVC_MAX_PPS)
    return AL_UNSUPPORTED;

  AL_THevcAup* aup = &IAup->hevcAup;

  aup->pPPS[PpsId].bConceal = (result != AL_OK);

  if(!aup->pPPS[PpsId].bConceal)
  {
    pCtx->tConceal.bHasPPS = true;

    if(pCtx->tConceal.iLastPPSId <= PpsId)
      pCtx->tConceal.iLastPPSId = PpsId;
  }

  return result;
}

/*****************************************************************************/
static bool isActiveSPSChanging(AL_THevcSps* pNewSPS, AL_THevcSps* pActiveSPS)
{
  // Only check resolution change - but any change is forgiven, according to the specification
  return pActiveSPS != NULL &&
         pNewSPS->sps_seq_parameter_set_id == pActiveSPS->sps_seq_parameter_set_id &&
         (pNewSPS->pic_height_in_luma_samples != pActiveSPS->pic_height_in_luma_samples ||
          pNewSPS->pic_width_in_luma_samples != pActiveSPS->pic_width_in_luma_samples);
}

/*****************************************************************************/
static AL_PARSE_RESULT parseAndApplySPS(AL_TAup* pIAup, AL_TRbspParser* pRP, AL_TDecCtx* pCtx)
{
  AL_THevcSps tNewSPS;
  AL_PARSE_RESULT eParseResult = AL_HEVC_ParseSPS(pRP, &tNewSPS);

  if(eParseResult != AL_BAD_ID)
  {
    if(eParseResult == AL_OK)
    {
      if(AL_Default_Decoder_HasOngoingFrame(pCtx) && isActiveSPSChanging(&tNewSPS, pIAup->hevcAup.pActiveSPS))
      {
        // An active SPS should not be modified unless it is the end of the CVS (spec 7.4.2.4).
        // So we consider we received the full frame.
        finishPreviousFrame(pCtx);

        eParseResult = AL_LAUNCHED_OK;
      }

      pIAup->hevcAup.pSPS[tNewSPS.sps_seq_parameter_set_id] = tNewSPS;
    }
    else
      pIAup->hevcAup.pSPS[tNewSPS.sps_seq_parameter_set_id].bConceal = true;
  }
  return eParseResult;
}

/*****************************************************************************/
static AL_NonVclNuts getNonVclNuts(void)
{
  AL_NonVclNuts nuts =
  {
    AL_HEVC_NUT_ERR, // NUT does not exist in HEVC
    AL_HEVC_NUT_VPS,
    AL_HEVC_NUT_SPS,
    AL_HEVC_NUT_PPS,
    AL_HEVC_NUT_FD,
    AL_HEVC_NUT_ERR, // NUT does not exist in HEVC
    AL_HEVC_NUT_ERR, // NUT does not exist in HEVC
    AL_HEVC_NUT_ERR, // NUT does not exist in HEVC
    AL_HEVC_NUT_PREFIX_SEI,
    AL_HEVC_NUT_SUFFIX_SEI,
    AL_HEVC_NUT_EOS,
    AL_HEVC_NUT_EOB,
  };
  return nuts;
}

/*****************************************************************************/
static bool isNutError(AL_ENut nut)
{
  if(nut >= AL_HEVC_NUT_ERR)
    return true;
  return false;
}

/*****************************************************************************/
static bool canNalBeReordered(AL_ENut nut)
{
  switch(nut)
  {
  case AL_HEVC_NUT_SUFFIX_SEI:
  case AL_HEVC_NUT_RSV_NVCL45:
  case AL_HEVC_NUT_RSV_NVCL46:
  case AL_HEVC_NUT_RSV_NVCL47:
  case AL_HEVC_NUT_UNSPEC_56:
  case AL_HEVC_NUT_UNSPEC_57:
  case AL_HEVC_NUT_UNSPEC_58:
  case AL_HEVC_NUT_UNSPEC_59:
  case AL_HEVC_NUT_UNSPEC_60:
  case AL_HEVC_NUT_UNSPEC_61:
  case AL_HEVC_NUT_UNSPEC_62:
  case AL_HEVC_NUT_UNSPEC_63:
    return true;
  default:
    return false;
  }
}

/*****************************************************************************/
void AL_HEVC_InitParser(AL_NalParser* pParser)
{
  pParser->parseDps = NULL;
  pParser->parseVps = AL_HEVC_ParseVPS;
  pParser->parseSps = parseAndApplySPS;
  pParser->parsePps = parsePPSandUpdateConcealment;
  pParser->parseAps = NULL;
  pParser->parsePh = NULL;
  pParser->parseSei = AL_HEVC_ParseSEI;
  pParser->decodeSliceData = decodeSliceData;
  pParser->isSliceData = isSliceData;
  pParser->finishPendingRequest = finishPreviousFrame;
  pParser->getNonVclNuts = getNonVclNuts;
  pParser->isNutError = isNutError;
  pParser->canNalBeReordered = canNalBeReordered;
}

/*****************************************************************************/
void AL_HEVC_InitAUP(AL_THevcAup* pAUP)
{
  for(int i = 0; i < AL_HEVC_MAX_PPS; ++i)
    pAUP->pPPS[i].bConceal = true;

  for(int i = 0; i < AL_HEVC_MAX_SPS; ++i)
    pAUP->pSPS[i].bConceal = true;

  pAUP->pActiveSPS = NULL;
}

/*****************************************************************************/
AL_ERR CreateHevcDecoder(AL_TDecoder** hDec, AL_IDecScheduler* pScheduler, AL_TAllocator* pAllocator, AL_TDecSettings* pSettings, AL_TDecCallBacks* pCB)
{
  pSettings->tStream.iMaxRef = Min(MAX_REF, pSettings->tStream.iMaxRef);
  AL_ERR errorCode = AL_CreateDefaultDecoder((AL_TDecoder**)hDec, pScheduler, pAllocator, pSettings, pCB);

  if(!AL_IS_ERROR_CODE(errorCode))
  {
    AL_TDecoder* pDec = *hDec;
    AL_TDecCtx* pCtx = &pDec->ctx;

    AL_HEVC_InitParser(&pCtx->parser);
  }

  return errorCode;
}
