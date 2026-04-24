// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "FrameParam.h"
#include "I_DecoderCtx.h"

#include "DefaultDecoder.h"
#include "SliceDataParsing.h"
#include "NalUnitParser.h"
#include "NalUnitParserPrivate.h"

#include "lib_common/Utils.h"
#include "lib_common/AvcLevelsLimit.h"
#include "lib_common/AvcUtils.h"
#include "lib_common/Error.h"
#include "lib_common/SyntaxConversion.h"

#include "lib_common_dec/RbspParser.h"
#include "lib_common_dec/DecInfo.h"
#include "lib_common_dec/DecInfoInternal.h"
#include "lib_common_dec/Defines_mcu.h"
#include "lib_common_dec/DecHardwareConfig.h"
#include "lib_common_dec/StreamSettingsInternal.h"
#include "lib_common_dec/HDRMeta.h"

#include "lib_decode/I_DecSchedulerInfo.h"

#include "lib_parsing/AvcParser.h"
#include "lib_parsing/Avc_PictMngr.h"
#include "lib_parsing/Avc_SliceHeaderParsing.h"

#include "lib_rtos/message.h"

/******************************************************************************/
static AL_TDimension extractDimension(AL_TAvcSps const* pSPS, bool bHasFields)
{
  int height = AL_AVC_GetFrameHeight(pSPS, bHasFields);

  return (AL_TDimension) {
           (pSPS->pic_width_in_mbs_minus1 + 1) * 16, height * 16
  };
}

/*************************************************************************/
/*
static int getMaxBitDepthFromSPSProfile(AL_TAvcSps const* pSPS)
{
  int profile_idc = pSPS->profile_idc;

  if(
    (profile_idc == AL_GET_PROFILE_IDC(AL_PROFILE_AVC_BASELINE))
    || (profile_idc == AL_GET_PROFILE_IDC(AL_PROFILE_AVC_MAIN))
    || (profile_idc == AL_GET_PROFILE_IDC(AL_PROFILE_AVC_EXTENDED))
    || (profile_idc == AL_GET_PROFILE_IDC(AL_PROFILE_AVC_HIGH))
    )
    return 8;
  else if((profile_idc == AL_GET_PROFILE_IDC(AL_PROFILE_AVC_HIGH10))
          || profile_idc == AL_GET_PROFILE_IDC(AL_PROFILE_AVC_HIGH_422))
    return 10;

  return 12;
}
*/

/*************************************************************************/
static int getMaxBitDepthFromSPSPBitDepth(AL_TAvcSps const* pSPS)
{
  int iSPSLumaBitDepth = pSPS->bit_depth_luma_minus8 + 8;
  int iSPSChromaBitDepth = pSPS->bit_depth_chroma_minus8 + 8;
  int iMaxSPSBitDepth = Max(iSPSLumaBitDepth, iSPSChromaBitDepth);
  int iMaxBitDepth = iMaxSPSBitDepth;

  if((iMaxBitDepth % 2) != 0)
    iMaxBitDepth++;

  return iMaxBitDepth;
}

/*************************************************************************/
static int getMaxNumberOfSlices(AL_TDecCtx const* pCtx, AL_TAvcSps const* pSPS)
{
  AL_TStreamSettings const* pStreamSettings = &pCtx->tCurrentStreamSettings;
  int macroblocksCountInPicture = GetSquareBlkNumber(pStreamSettings->tDim, 16);
  int numUnitsInTick = 1, timeScale = 1;

  if(pSPS->vui_parameters_present_flag && pSPS->vui_param.vui_timing_info_present_flag)
  {
    numUnitsInTick = pSPS->vui_param.vui_num_units_in_tick;
    timeScale = pSPS->vui_param.vui_time_scale;
  }

  if(pCtx->bForceFrameRate)
  {
    numUnitsInTick = pCtx->pChanParam->uClkRatio;
    timeScale = 2 * pCtx->pChanParam->uFrameRate;
  }
  else if((pCtx->uConcealMaxFps > 0) && ((timeScale / (2 * numUnitsInTick)) > pCtx->uConcealMaxFps))
  {
    numUnitsInTick = 1;
    timeScale = 2 * pCtx->uConcealMaxFps;
  }

  if(numUnitsInTick == 0 || timeScale == 0)
    return Min(macroblocksCountInPicture, AL_AVC_GetSpecificationMaxNumberOfSlices());

  return AL_AVC_GetMaxNumberOfSlices(pStreamSettings->eProfile, pStreamSettings->iLevel, numUnitsInTick, timeScale, macroblocksCountInPicture);
}

/*****************************************************************************/
static AL_ERR isSPSCompatibleWithHardware(AL_TAvcSps const* pSPS, bool bHasFields)
{
  int iSPSMaxBitDepth = getMaxBitDepthFromSPSPBitDepth(pSPS);

  if(iSPSMaxBitDepth > AL_HWConfig_Dec_GetSupportedBitDepth())
  {
    VIDEO_ERROR_PRINT("Bitdepth '%i' is not supported by the HARDWARE. Maximum supported bitdepth is '%i'.", iSPSMaxBitDepth, AL_HWConfig_Dec_GetSupportedBitDepth());
    return AL_WARN_SPS_BITDEPTH_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  AL_TDimension tSPSDim = extractDimension(pSPS, bHasFields);

  int32_t iMinResolution = AL_CORE_MIN_CU_NB << AL_CORE_AVC_LOG2_MAX_CU_SIZE;

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

  return AL_SUCCESS;
}

/*****************************************************************************/
static AL_ERR isSPSCompatibleWithInitialStreamSettings(AL_TDecCtx const* pCtx, AL_TAvcSps const* pSPS, bool bHasFields)
{
  AL_TStreamSettings const* pStreamSettings = &pCtx->tInitialStreamSettings;

  if(!CheckStreamSettings(pStreamSettings))
    return AL_ERR_REQUEST_MALFORMED;

  int iSPSLumaBitDepth = pSPS->bit_depth_luma_minus8 + 8;

  if(pStreamSettings->iBitDepth < iSPSLumaBitDepth)
  {
    VIDEO_ERROR_PRINT("Bitdepth '%i' is not supported by the CHANNEL. Maximum supported bitdepth is '%i'.", iSPSLumaBitDepth, pStreamSettings->iBitDepth);
    return AL_WARN_SPS_BITDEPTH_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  int iSPSChromaBitDepth = pSPS->bit_depth_chroma_minus8 + 8;

  if(pStreamSettings->iBitDepth < iSPSChromaBitDepth)
  {
    VIDEO_ERROR_PRINT("Bitdepth '%i' is not supported by the CHANNEL. Maximum supported bitdepth is '%i'.", iSPSLumaBitDepth, pStreamSettings->iBitDepth);
    return AL_WARN_SPS_BITDEPTH_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;
  }

  AL_TDimension tSPSDim = extractDimension(pSPS, bHasFields);

  int iSPSLevel = pSPS->constraint_set3_flag ? 9 : pSPS->level_idc; /* We treat constraint set 3 as a level 9 */
  int iCurDPBSize = Max(AL_AVC_GetMaxDPBSize(pStreamSettings->iLevel, pStreamSettings->tDim.iWidth, pStreamSettings->tDim.iHeight, 0, false, false), pStreamSettings->iMaxRef);
  int iNewDPBSize = Min(AL_AVC_GetMaxDPBSize(iSPSLevel, tSPSDim.iWidth, tSPSDim.iHeight, 0, false, false), pSPS->max_num_ref_frames);

  if(iNewDPBSize > iCurDPBSize)
    return AL_WARN_SPS_LEVEL_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;

  AL_EChromaMode eSPSChromaMode = (AL_EChromaMode)pSPS->chroma_format_idc;

  if(pStreamSettings->eChroma < eSPSChromaMode)
  {
    VIDEO_ERROR_PRINT("Chroma mode '%i' is not supported by the CHANNEL. Maximum chroma mode supported is '%i'.", eSPSChromaMode, pStreamSettings->eChroma);
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

  if((pStreamSettings->eSequenceMode != AL_SM_UNKNOWN) && (pStreamSettings->eSequenceMode != AL_SM_PROGRESSIVE))
    return AL_WARN_SPS_INTERLACE_NOT_COMPATIBLE_WITH_CHANNEL_SETTINGS;

  return AL_SUCCESS;
}

/******************************************************************************/
static void extractStreamSettings(AL_TAvcSps const* pSPS, AL_TStreamSettings* pStreamSettings, bool bHasFields)
{
  uint32_t uFlags = (pSPS->constraint_set0_flag |
                     (pSPS->constraint_set1_flag << 1) |
                     (pSPS->constraint_set2_flag << 2) |
                     (pSPS->constraint_set3_flag << 3) |
                     (pSPS->constraint_set4_flag << 4) |
                     (pSPS->constraint_set5_flag << 5));

  pStreamSettings->tDim = extractDimension(pSPS, bHasFields);
  pStreamSettings->eChroma = (AL_EChromaMode)pSPS->chroma_format_idc;
  pStreamSettings->iBitDepth = getMaxBitDepthFromSPSPBitDepth(pSPS);
  pStreamSettings->iLevel = pSPS->constraint_set3_flag ? 9 : pSPS->level_idc;
  pStreamSettings->eProfile = AL_PROFILE_AVC | pSPS->profile_idc | AL_CS_FLAGS(uFlags);
  pStreamSettings->eSequenceMode = AL_SM_PROGRESSIVE;
  pStreamSettings->iMaxRef = 0;
}

/******************************************************************************/
static AL_ERR resolutionFound(AL_TDecCtx* pCtx, AL_TStreamSettings const* pCurrentStreamSettings, AL_TCropInfo const* pCropInfo, int iSPSMaxRefFrames)
{
  int iDpbMaxBuf = AL_AVC_GetMaxDpbBuffers(pCurrentStreamSettings, iSPSMaxRefFrames);
  int iMaxBuf = AVC_GetMinOutputBuffersNeeded(iDpbMaxBuf, pCtx->iStackSize, pCurrentStreamSettings->bDecodeIntraOnly);

  return pCtx->tDecCB.resolutionFoundCB.func(iMaxBuf, pCurrentStreamSettings, pCropInfo, pCtx->tDecCB.resolutionFoundCB.userParam);
}

/******************************************************************************/
static bool allocateBuffers(AL_TDecCtx* pCtx, AL_TAvcSps const* pSPS, bool bHasFields)
{
  int const iNumMBs = (pSPS->pic_width_in_mbs_minus1 + 1) * AL_AVC_GetFrameHeight(pSPS, bHasFields);
  (void)iNumMBs;
  AL_TStreamSettings const* pStreamSettings = &pCtx->tCurrentStreamSettings;
  DBG_ASSERT_COND(iNumMBs == ((pStreamSettings->tDim.iWidth / 16) * (pStreamSettings->tDim.iHeight / 16)));

  int iSPSMaxSlices = getMaxNumberOfSlices(pCtx, pSPS) + 1; // One more for conceal slice
  int iSizeWP = iSPSMaxSlices * WP_SLICE_SIZE;
  int iSizeSP = iSPSMaxSlices * sizeof(AL_TDecSliceParam);
  int iSizeCompData = AL_GetAllocSize_AvcCompData(pStreamSettings->tDim, pStreamSettings->eChroma);
  int iSizeCompMap = AL_GetAllocSize_DecCompMap(pStreamSettings->tDim);
  AL_ERR error = AL_ERR_NO_MEMORY;

  if(!AL_Default_Decoder_AllocPool(pCtx, 0, 0, iSizeWP, iSizeSP, iSizeCompData, iSizeCompMap, 0))
    goto fail_alloc;

  int iDpbMaxBuf = AL_AVC_GetMaxDpbBuffers(pStreamSettings, pSPS->max_num_ref_frames * (1 + bHasFields));
  int iMaxBuf = AL_AVC_GetMinOutputBuffersNeeded(pStreamSettings, pCtx->iStackSize);
  int iSizeMV = AL_GetAllocSize_AvcMV(pStreamSettings->tDim);
  int iSizePOC = POCBUFF_PL_SIZE;

  if(!AL_Default_Decoder_AllocMv(pCtx, iSizeMV, iSizePOC, iMaxBuf))
    goto fail_alloc;

  AL_TPictMngrParam tPictMngrParam;
  tPictMngrParam.iNumDPBRef = iDpbMaxBuf;
  tPictMngrParam.eDPBMode = pCtx->eDpbMode;
  tPictMngrParam.eFbStorageMode = pCtx->pChanParam->eFBStorageMode;
  tPictMngrParam.iNumMV = iMaxBuf;
  tPictMngrParam.iSizeMV = iSizeMV;
  tPictMngrParam.bForceOutput = pCtx->pChanParam->bUseEarlyCallback;
  tPictMngrParam.tOutputPosition = pCtx->tOutputPosition;

  if(!AL_PictMngr_BasicInit(&pCtx->PictMngr, &tPictMngrParam))
    goto fail_alloc;

  AL_TCropInfo tCropInfo;
  AL_AVC_GetCropInfo(pSPS, &tCropInfo);
  error = resolutionFound(pCtx, pStreamSettings, &tCropInfo, pSPS->max_num_ref_frames * (1 + bHasFields));

  if(AL_IS_ERROR_CODE(error))
    goto fail_alloc;

  Rtos_WaitEvent(pCtx->hDecOutSettingsConfiguredEvt, AL_WAIT_FOREVER);

  return true;

  fail_alloc:
  AL_Default_Decoder_SetError(pCtx, error, -1, true);
  return false;
}

/******************************************************************************/
static bool initChannel(AL_TDecCtx* pCtx, AL_TAvcSps const* pSPS)
{
  AL_TDecChanParam* pChan = pCtx->pChanParam;
  AL_TStreamSettings const* pStreamSettings = &pCtx->tCurrentStreamSettings;
  pChan->iWidth = pStreamSettings->tDim.iWidth;
  pChan->iHeight = pStreamSettings->tDim.iHeight;
  pChan->uLog2MaxCuSize = AL_CORE_AVC_LOG2_MAX_CU_SIZE;
  pChan->eMaxChromaMode = pStreamSettings->eChroma;

  int const iSPSMaxSlices = getMaxNumberOfSlices(pCtx, pSPS);
  pChan->iMaxSlices = iSPSMaxSlices;
  pChan->iMaxTiles = 1;

  if(!pCtx->bForceFrameRate && pSPS->vui_parameters_present_flag && pSPS->vui_param.vui_timing_info_present_flag)
  {
    pChan->uFrameRate = pSPS->vui_param.vui_time_scale / 2;
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
static int slicePpsId(AL_TAvcSliceHdr const* pSlice)
{
  return pSlice->pic_parameter_set_id;
}

/******************************************************************************/
static int sliceSpsId(AL_TAvcPps const* pPps, AL_TAvcSliceHdr const* pSlice)
{
  int const ppsid = slicePpsId(pSlice);
  return pPps[ppsid].seq_parameter_set_id;
}

/******************************************************************************/
static bool initSlice(AL_TDecCtx* pCtx, AL_TAvcSliceHdr* pSlice)
{
  AL_TAvcAup* aup = &pCtx->aup.avcAup;

  if(!pCtx->bIsFirstSPSChecked)
  {
    AL_ERR const ret = !pCtx->bAreBuffersAllocated ? isSPSCompatibleWithHardware(pSlice->pSPS, pSlice->field_pic_flag) : isSPSCompatibleWithInitialStreamSettings(pCtx, pSlice->pSPS, pSlice->field_pic_flag);

    if(ret != AL_SUCCESS)
    {
      VIDEO_ERROR_PRINT("Cannot decode using the current allocated buffers");
      pSlice->pPPS = &aup->pPPS[pCtx->tConceal.iLastPPSId];
      pSlice->pSPS = pSlice->pPPS->pSPS;
      AL_Default_Decoder_SetError(pCtx, ret, -1, true);
      return false;
    }

    if(!pCtx->bAreBuffersAllocated)
    {
      extractStreamSettings(pSlice->pSPS, &pCtx->tCurrentStreamSettings, pSlice->field_pic_flag);
      pCtx->tInitialStreamSettings = pCtx->tCurrentStreamSettings;
    }

    pCtx->bIsFirstSPSChecked = true;

    if(!pCtx->bAreBuffersAllocated)
    {
      aup->pActiveSPS = pSlice->pSPS;

      if(!allocateBuffers(pCtx, pSlice->pSPS, pSlice->field_pic_flag))
        return false;
    }

    if(!initChannel(pCtx, pSlice->pSPS))
      return false;

    pCtx->bAreBuffersAllocated = true;
  }

  int const spsid = sliceSpsId(aup->pPPS, pSlice);
  aup->pActiveSPS = &aup->pSPS[spsid];

  return true;
}

/*****************************************************************************/
static void copyScalingList(AL_TAvcPps* pPPS, AL_TScl* pSCL)
{
  Rtos_Memcpy((*pSCL)[0].t4x4Y,
              !pPPS->UseDefaultScalingMatrix4x4Flag[0] ? pPPS->ScalingList4x4[0] :
              AL_AVC_DefaultScalingLists4x4[0], 16);
  Rtos_Memcpy((*pSCL)[0].t4x4Cb,
              !pPPS->UseDefaultScalingMatrix4x4Flag[1] ? pPPS->ScalingList4x4[1] :
              AL_AVC_DefaultScalingLists4x4[0], 16);
  Rtos_Memcpy((*pSCL)[0].t4x4Cr,
              !pPPS->UseDefaultScalingMatrix4x4Flag[2] ? pPPS->ScalingList4x4[2] :
              AL_AVC_DefaultScalingLists4x4[0], 16);
  Rtos_Memcpy((*pSCL)[1].t4x4Y,
              !pPPS->UseDefaultScalingMatrix4x4Flag[3] ? pPPS->ScalingList4x4[3] :
              AL_AVC_DefaultScalingLists4x4[1], 16);
  Rtos_Memcpy((*pSCL)[1].t4x4Cb,
              !pPPS->UseDefaultScalingMatrix4x4Flag[4] ? pPPS->ScalingList4x4[4] :
              AL_AVC_DefaultScalingLists4x4[1], 16);
  Rtos_Memcpy((*pSCL)[1].t4x4Cr,
              !pPPS->UseDefaultScalingMatrix4x4Flag[5] ? pPPS->ScalingList4x4[5] :
              AL_AVC_DefaultScalingLists4x4[1], 16);
  Rtos_Memcpy((*pSCL)[0].t8x8Y,
              !pPPS->UseDefaultScalingMatrix8x8Flag[0] ? pPPS->ScalingList8x8[0] :
              AL_AVC_DefaultScalingLists8x8[0], 64);
  Rtos_Memcpy((*pSCL)[1].t8x8Y,
              !pPPS->UseDefaultScalingMatrix8x8Flag[1] ? pPPS->ScalingList8x8[1] :
              AL_AVC_DefaultScalingLists8x8[1], 64);
}

/******************************************************************************/
static void processScalingList(AL_TAvcAup* pAUP, AL_TAvcSliceHdr* pSlice, AL_TScl* pScl)
{
  int ppsid = pSlice->pic_parameter_set_id;

  AL_CleanupMemory(pScl, sizeof(*pScl));
  copyScalingList(&pAUP->pPPS[ppsid], pScl);
}

/******************************************************************************/
static void concealSlice(AL_TDecCtx* pCtx, AL_TDecPicParam* pPP, AL_TDecSliceParam* pSP, AL_TAvcSliceHdr* pSlice, AL_ENut eNUT)
{
  AL_AVC_FillPictParameters(pSlice, pCtx, pPP);
  AL_AVC_FillSliceParameters(pSlice, pCtx, pSP, pPP, true);

  pSP->eSliceType = AL_SLICE_CONCEAL;
  AL_Default_Decoder_SetError(pCtx, AL_WARN_CONCEAL_DETECT, pPP->tBufIDs.FrmID, true);

  if(eNUT == AL_AVC_NUT_VCL_IDR)
  {
    pCtx->PictMngr.iCurFramePOC = 0;
    pSP->ValidConceal = false;
  }
  else
  {
    AL_SET_DEC_OPT(pPP, IntraOnly, 0);
    AL_SetConcealParameters(pCtx, pSP);
  }
}

/*****************************************************************************/
static void createConcealSlice(AL_TDecCtx* pCtx, AL_TDecPicParam* pPP, AL_TDecSliceParam* pSP, AL_TAvcSliceHdr* pSlice)
{
  uint8_t uCurSliceType = pSlice->slice_type;

  concealSlice(pCtx, pPP, pSP, pSlice, false);
  pSP->FirstLcuSliceSegment = 0;
  pSP->FirstLcuSlice = 0;
  pSP->SliceFirstLCU = 0;
  pSP->NextSliceSegment = pSlice->first_mb_in_slice;
  pSP->SliceNumLCU = pSlice->first_mb_in_slice;

  pSlice->slice_type = uCurSliceType;
}

/*****************************************************************************/
static void updatePictManager(AL_TPictMngrCtx* pCtx, AL_ENut eNUT, AL_TAvcSliceHdr* pSlice)
{
  bool bClearRef = AL_AVC_IsIDR(eNUT);
  AL_EMarkingRef eMarkingFlag = ((pSlice->nal_ref_idc > 0) || bClearRef) ? (pSlice->long_term_reference_flag ? LONG_TERM_REF : SHORT_TERM_REF) : UNUSED_FOR_REF;

  // update picture manager
  pCtx->iPrevFrameNum = pSlice->frame_num;
  pCtx->bLastIsIDR = bClearRef;

  AL_AVC_PictMngr_EndParsing(pCtx, bClearRef, eMarkingFlag);

  if(pSlice->nal_ref_idc)
  {
    AL_Dpb_MarkingProcess(&pCtx->DPB, pSlice, pCtx->iCurFramePOC);

    if(AL_Dpb_LastHasMMCO5(&pCtx->DPB))
    {
      pCtx->iPrevFrameNum = 0;
      pCtx->iCurFramePOC = 0;
    }
  }
  AL_AVC_PictMngr_CleanDPB(pCtx);
}

/******************************************************************************/
static void reallyEndFrame(AL_TDecCtx* pCtx, AL_ENut eNUT, AL_TAvcSliceHdr* pSlice, bool hasPreviousSlice)
{
  updatePictManager(&pCtx->PictMngr, eNUT, pSlice);

  if(pCtx->pChanParam->eDecUnit == AL_AU_UNIT)
    AL_LaunchFrameDecoding(pCtx);

  if(pCtx->pChanParam->eDecUnit == AL_VCL_NAL_UNIT)
    AL_LaunchSliceDecoding(pCtx, true, hasPreviousSlice);

  UpdateContextAtEndOfFrame(pCtx);
}

static void endFrame(AL_TDecCtx* pCtx, AL_ENut eNUT, AL_TAvcSliceHdr* pSlice)
{
  reallyEndFrame(pCtx, eNUT, pSlice, true);
}

static void endFrameConceal(AL_TDecCtx* pCtx, AL_ENut eNUT, AL_TAvcSliceHdr* pSlice)
{
  reallyEndFrame(pCtx, eNUT, pSlice, false);
}

/*****************************************************************************/
static void finishPreviousFrame(AL_TDecCtx* pCtx)
{
  AL_TAvcSliceHdr* pSlice = &pCtx->AvcSliceHdr[pCtx->uCurID];
  AL_TDecPicParam* pPP = &pCtx->PoolPP[pCtx->uToggle];
  AL_TDecSliceParam* pSP = &(((AL_TDecSliceParam*)pCtx->PoolSP[pCtx->uToggle].tMD.pVirtualAddr)[pCtx->tCurrentFrameCtx.uNumSlice - 1]);
  AL_TDecPicBuffers* pBufs = &pCtx->PoolPB[pCtx->uToggle];

  /* AVC doesn't have Dependent */
  bool const bUsedDependent = false;
  AL_TerminatePreviousCommand(pCtx, pPP, pSP, pBufs, true, bUsedDependent);

  // copy stream offset from previous command
  pCtx->iStreamOffset[pCtx->iNumFrmBlk1 % pCtx->iStackSize] = pCtx->iStreamOffset[(pCtx->iNumFrmBlk1 + pCtx->iStackSize - 1) % pCtx->iStackSize];

  /* The slice is its own previous slice as we changed it in the last slice
   * This means that in AL_VCL_NAL_UNIT we don't want to send a previous slice at all. */
  endFrameConceal(pCtx, pSlice->nal_unit_type, pSlice);

  AL_DecFrameCtx_Reset(&pCtx->tCurrentFrameCtx);
}

/******************************************************************************/
static bool constructRefPicList(AL_TAvcSliceHdr* pSlice, AL_TDecCtx* pCtx, TBufferListRef* pListRef)
{
  AL_TPictMngrCtx* pPictMngrCtx = &pCtx->PictMngr;

  AL_Dpb_PictNumberProcess(&pPictMngrCtx->DPB, pSlice);

  AL_AVC_PictMngr_InitPictList(pPictMngrCtx, pSlice, pListRef);
  AL_AVC_PictMngr_ReorderPictList(pPictMngrCtx, pSlice, pListRef);

  for(int i = pSlice->num_ref_idx_l0_active_minus1 + 1; i < MAX_REF; ++i)
    (*pListRef)[0][i].uNodeID = 0xFF;

  for(int i = pSlice->num_ref_idx_l1_active_minus1 + 1; i < MAX_REF; ++i)
    (*pListRef)[1][i].uNodeID = 0xFF;

  int pNumRef[2] = { 0, 0 };

  for(int i = 0; i < MAX_REF; ++i)
  {
    if((*pListRef)[0][i].uNodeID != uEndOfList)
      pNumRef[0]++;

    if((*pListRef)[1][i].uNodeID != uEndOfList)
      pNumRef[1]++;
  }

  if((pSlice->slice_type != AL_SLICE_I && pNumRef[0] < pSlice->num_ref_idx_l0_active_minus1 + 1) ||
     (pSlice->slice_type == AL_SLICE_B && pNumRef[1] < pSlice->num_ref_idx_l1_active_minus1 + 1))
    return false;

  if(((pNumRef[0] + pNumRef[1]) > 0) && (AL_AVC_PictMngr_GetNumExistingRef(pPictMngrCtx, (TBufferListRef const*)pListRef) == 0))
    return false;

  return true;
}

/*****************************************************************************/
static bool isRandomAccessPoint(AL_ENut eNUT)
{
  return eNUT == AL_AVC_NUT_VCL_IDR;
}

/*****************************************************************************/
static bool isValidSyncPoint(AL_TDecCtx* pCtx, AL_ENut eNUT, AL_ESliceType ePicType, int32_t iRecoveryCnt)
{
  if(isRandomAccessPoint(eNUT))
    return true;

  if(iRecoveryCnt != 0)
    return true;

  if(pCtx->bUseIFramesAsSyncPoint && eNUT == AL_AVC_NUT_VCL_NON_IDR && ePicType == AL_SLICE_I)
    return true;

  return false;
}

/*****************************************************************************/
static bool avcInitFrameBuffers(AL_TDecCtx* pCtx, bool bStartsNewCVS, const AL_TAvcSps* pSPS, AL_TDecPicParam* pPP, bool bHasFields, AL_TDecPicBuffers* pBufs)
{
  (void)bStartsNewCVS;
  AL_TDimension const tDim = extractDimension(pSPS, bHasFields);

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
static void avcGetCropInfo(AL_TDecCtx* pCtx, AL_TAvcSps const* pSPS, AL_TCropInfo* pCropInfo)
{
  (void)pCtx;

  AL_AVC_GetCropInfo(pSPS, pCropInfo);
}

/*****************************************************************************/
static bool decodeSliceData(AL_TAup* pIAUP, AL_TDecCtx* pCtx, AL_ENut eNUT, bool bIsLastAUNal, int* iNumSlice)
{
  if(pCtx->tCurrentFrameCtx.bFirstSliceValid && *iNumSlice > pCtx->pChanParam->iMaxSlices)
    return false;

  // Slice header deanti-emulation
  AL_TRbspParser rp;
  TCircBuffer* pBufStream = &pCtx->Stream;
  InitRbspParser(pBufStream, pCtx->BufNoAE.tMD.pVirtualAddr, pCtx->BufNoAE.tMD.uSize, true, &rp);

  // Parse Slice Header
  uint8_t uToggleID = (~pCtx->uCurID) & 0x01;
  AL_TAvcSliceHdr* pSlice = &pCtx->AvcSliceHdr[uToggleID];
  Rtos_Memset(pSlice, 0, sizeof(AL_TAvcSliceHdr));
  AL_TConceal* pConceal = &pCtx->tConceal;
  AL_TAvcAup* pAUP = &pIAUP->avcAup;

  AL_ERR const ret = AL_AVC_ParseSliceHeader(pSlice, &rp, pConceal, pAUP->pPPS);
  bool isSliceHdrValid = ret == AL_SUCCESS;
  bool isValid = isSliceHdrValid;
  bool bSliceBelongsToSameFrame = true;

  if(!pSlice->pSPS)
    pSlice->pSPS = pAUP->pActiveSPS;

  if(isValid)
  {
    if(pSlice->pic_order_cnt_lsb != pCtx->iCurPocLsb && pSlice->first_mb_in_slice != 0)
      bSliceBelongsToSameFrame = false;
  }
  else if(ret != AL_WARN_CONCEAL_DETECT)
    AL_Default_Decoder_SetError(pCtx, ret, -1, true);

  if(pSlice->first_mb_in_slice <= pConceal->iFirstLCU)
    bSliceBelongsToSameFrame = false;

  bool* bFirstIsValid = &pCtx->bFirstIsValid;
  AL_TDecFrameCtx* pFrmCtx = &pCtx->tCurrentFrameCtx;

  if(!bSliceBelongsToSameFrame && AL_Default_Decoder_HasOngoingFrame(pCtx))
  {
    finishPreviousFrame(pCtx);

    if(pCtx->eInputMode == AL_DEC_SPLIT_INPUT)
    {
      pConceal->bSkipRemainingNals = true;
      return true;
    }
  }

  pFrmCtx->bIsIntraOnly &= pSlice->slice_type == AL_SLICE_I;

  if(isValid)
    pConceal->iFirstLCU = pSlice->first_mb_in_slice;

  AL_TDecPicBuffers* pBufs = &pCtx->PoolPB[pCtx->uToggle];
  AL_TDecPicParam* pPP = &pCtx->PoolPP[pCtx->uToggle];

  if(isValid && pCtx->bAreBuffersAllocated)
  {
    AL_TAvcSps* pSPS = pSlice->pSPS;
    AL_ERR const ret = isSPSCompatibleWithInitialStreamSettings(pCtx, pSPS, pSlice->field_pic_flag);
    isValid = ret == AL_SUCCESS;
    AL_TStreamSettings spsSettings;

    if(isValid)
    {
      spsSettings.bDecodeIntraOnly = pCtx->tCurrentStreamSettings.bDecodeIntraOnly;
      extractStreamSettings(pSPS, &spsSettings, pSlice->field_pic_flag);
      // get value from pre alloc
      spsSettings.iMaxRef = pCtx->tCurrentStreamSettings.iMaxRef;
    }

    if(!isValid)
    {
      VIDEO_ERROR_PRINT("SPS is not compatible with the current allocated buffers");
      AL_Default_Decoder_SetError(pCtx, ret, pPP->tBufIDs.FrmID, true);
      pSPS->bConceal = true;
    }
    else if(spsSettings.tDim.iWidth != pCtx->tCurrentStreamSettings.tDim.iWidth || spsSettings.tDim.iHeight != pCtx->tCurrentStreamSettings.tDim.iHeight)
    {
      AL_TCropInfo tCropInfo;
      AL_AVC_GetCropInfo(pSPS, &tCropInfo);
      int iSPSMaxRefFrames = Max(pSPS->max_num_ref_frames, spsSettings.iMaxRef);
      AL_ERR error = resolutionFound(pCtx, &spsSettings, &tCropInfo, iSPSMaxRefFrames * (1 + pSlice->field_pic_flag));

      if(error != AL_SUCCESS)
      {
        VIDEO_ERROR_PRINT("Resolution change detected but resolutionFound callback returns with error");
        AL_Default_Decoder_SetError(pCtx, AL_WARN_RES_FOUND_CB, pPP->tBufIDs.FrmID, false);
        pSPS->bConceal = true;
        isValid = false;
      }
      pCtx->tCurrentStreamSettings = spsSettings;
    }
  }

  if(isValid)
  {
    pCtx->iCurPocLsb = pSlice->pic_order_cnt_lsb;

    if(!initSlice(pCtx, pSlice))
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
  }

  pCtx->uCurID = (pCtx->uCurID + 1) & 1;
  AL_TDecSliceParam* pSP = &(((AL_TDecSliceParam*)pCtx->PoolSP[pCtx->uToggle].tMD.pVirtualAddr)[pFrmCtx->uNumSlice]);

  if(isSliceHdrValid && !pSlice->pSPS->bConceal && pCtx->bAreBuffersAllocated &&
     !(pFrmCtx->bFirstSliceValid) && pSlice->first_mb_in_slice)
  {
    createConcealSlice(pCtx, pPP, pSP, pSlice);

    pSP = &(((AL_TDecSliceParam*)pCtx->PoolSP[pCtx->uToggle].tMD.pVirtualAddr)[++pFrmCtx->uNumSlice]);
    pFrmCtx->bFirstSliceValid = true;
  }

  if(isValid && pSlice->slice_type != AL_SLICE_I)
    AL_SET_DEC_OPT(pPP, IntraOnly, 0);

  pBufs->tStream.tMD = pCtx->Stream.tMD;

  // Compute Current POC
  if(isValid && (!pSlice->first_mb_in_slice || !bSliceBelongsToSameFrame))
  {
    AL_AVC_PictMngr_SetCurrentPOC(&pCtx->PictMngr, pSlice);
    AL_AVC_PictMngr_SetCurrentPicStruct(&pCtx->PictMngr, AL_AVC_GetPicStruct(pSlice));
  }

  // compute check gaps in frameNum
  if(isValid)
    AL_AVC_PictMngr_Fill_Gap_In_FrameNum(&pCtx->PictMngr, pSlice);

  if(pCtx->bAreBuffersAllocated && pFrmCtx->eBufStatus == DEC_FRAME_BUF_NONE && pSlice->pSPS)
  {
    if(!avcInitFrameBuffers(pCtx, isRandomAccessPoint(eNUT), pSlice->pSPS, pPP, pSlice->field_pic_flag, pBufs))
      return false;
    pFrmCtx->eBufStatus = DEC_FRAME_BUF_RESERVED;
    AL_TCropInfo tCropInfo;
    avcGetCropInfo(pCtx, pSlice->pSPS, &tCropInfo);
    AL_AVC_PictMngr_UpdateRecInfo(&pCtx->PictMngr, &tCropInfo, AL_AVC_GetPicStruct(pSlice));
  }

  bool bLastSlice = *iNumSlice >= pCtx->pChanParam->iMaxSlices;

  if(bLastSlice && !bIsLastAUNal)
    isValid = false;

  AL_TScl ScalingList = { 0 };

  if(pCtx->tCurrentStreamSettings.bDecodeIntraOnly && !pFrmCtx->bIsIntraOnly && bIsLastAUNal)
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

    // update Nal Unit size
    UpdateCircBuffer(&rp, pBufStream, &pSlice->slice_header_length);

    processScalingList(pAUP, pSlice, &ScalingList);

    if(pFrmCtx->uNumSlice == 0)
      AL_AVC_FillPictParameters(pSlice, pCtx, pPP);
    AL_AVC_FillSliceParameters(pSlice, pCtx, pSP, pPP, false);

    if(!constructRefPicList(pSlice, pCtx, &pCtx->ListRef) && (pIAUP->iRecoveryCnt == 0))
    {
      concealSlice(pCtx, pPP, pSP, pSlice, eNUT);
    }
    else
    {
      AL_AVC_FillSlicePicIdRegister(pCtx, pSP);
      pConceal->bValidFrame = true;
      AL_SetConcealParameters(pCtx, pSP);
    }
  }
  else if((bIsLastAUNal || !pSlice->first_mb_in_slice || bLastSlice) && (*bFirstIsValid) && pFrmCtx->bFirstSliceValid &&
          !(pCtx->tCurrentStreamSettings.bDecodeIntraOnly && !pFrmCtx->bIsIntraOnly)) /* conceal the current slice data */
  {
    concealSlice(pCtx, pPP, pSP, pSlice, eNUT);

    if(bLastSlice)
      pSP->NextSliceSegment = pPP->LcuPicWidth * pPP->LcuPicHeight;
  }
  else // skip slice
  {
    if(bIsLastAUNal || (pCtx->tCurrentStreamSettings.bDecodeIntraOnly && !pFrmCtx->bIsIntraOnly))
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

  if(isValid && !pSlice->first_mb_in_slice)
    pFrmCtx->bFirstSliceValid = true;

  // Launch slice decoding
  AL_AVC_PrepareCommand(pCtx, &ScalingList, pPP, pBufs, pSP, pSlice, bIsLastAUNal || bLastSlice, isValid);

  ++pFrmCtx->uNumSlice;
  ++(*iNumSlice);

  if(pAUP->ePictureType == AL_SLICE_I || pSlice->slice_type == AL_SLICE_B)
    pAUP->ePictureType = (AL_ESliceType)pSlice->slice_type;

  if(bIsLastAUNal || bLastSlice)
  {
    endFrame(pCtx, eNUT, pSlice);
    pAUP->ePictureType = AL_SLICE_I;
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
  case AL_AVC_NUT_VCL_NON_IDR:
  case AL_AVC_NUT_VCL_IDR:
    return true;
  default:
    return false;
  }
}

/*****************************************************************************/
static AL_PARSE_RESULT parsePPSandUpdateConcealment(AL_TAup* IAup, AL_TRbspParser* rp, AL_TDecCtx* pCtx)
{
  uint16_t PpsId;
  AL_PARSE_RESULT result = AL_AVC_ParsePPS(IAup, rp, &PpsId);

  if(PpsId >= AL_AVC_MAX_PPS)
    return AL_UNSUPPORTED;

  AL_TAvcAup* aup = &IAup->avcAup;

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
static bool isActiveSPSChanging(AL_TAvcSps* pNewSPS, AL_TAvcSps* pActiveSPS)
{
  // Only check resolution change - but any change is forgiven, according to the specification
  return pActiveSPS != NULL &&
         pNewSPS->seq_parameter_set_id == pActiveSPS->seq_parameter_set_id &&
         (pNewSPS->pic_width_in_mbs_minus1 != pActiveSPS->pic_width_in_mbs_minus1 ||
          pNewSPS->pic_height_in_map_units_minus1 != pActiveSPS->pic_height_in_map_units_minus1);
}

/*****************************************************************************/
static AL_PARSE_RESULT parseAndApplySPS(AL_TAup* pIAup, AL_TRbspParser* pRP, AL_TDecCtx* pCtx)
{
  AL_TAvcSps tNewSPS;
  AL_PARSE_RESULT eParseResult = AL_AVC_ParseSPS(pRP, &tNewSPS);

  if(eParseResult != AL_BAD_ID)
  {
    if(eParseResult == AL_OK)
    {
      if(AL_Default_Decoder_HasOngoingFrame(pCtx) && isActiveSPSChanging(&tNewSPS, pIAup->avcAup.pActiveSPS))
      {
        // An active SPS should not be modified unless it is the end of the CVS (spec I.7.4.1.2).
        // So we consider we received the full frame.
        finishPreviousFrame(pCtx);
        eParseResult = AL_LAUNCHED_OK;
      }
      pIAup->avcAup.pSPS[tNewSPS.seq_parameter_set_id] = tNewSPS;
    }
    else
      pIAup->avcAup.pSPS[tNewSPS.seq_parameter_set_id].bConceal = true;
  }

  return eParseResult;
}

/*****************************************************************************/
static AL_NonVclNuts getNonVclNuts(void)
{
  AL_NonVclNuts nuts =
  {
    AL_AVC_NUT_ERR, // NUT does not exist in AVC
    AL_AVC_NUT_ERR, // NUT does not exist in AVC
    AL_AVC_NUT_SPS,
    AL_AVC_NUT_PPS,
    AL_AVC_NUT_FD,
    AL_AVC_NUT_ERR, // NUT does not exist in AVC
    AL_AVC_NUT_ERR, // NUT does not exist in AVC
    AL_AVC_NUT_ERR, // NUT does not exist in AVC
    AL_AVC_NUT_PREFIX_SEI,
    AL_AVC_NUT_PREFIX_SEI, /* AVC doesn't distinguish between prefix and suffix */
    AL_AVC_NUT_EOS,
    AL_AVC_NUT_EOB,
  };
  return nuts;
}

/*****************************************************************************/
static bool isNutError(AL_ENut nut)
{
  if(nut >= AL_AVC_NUT_ERR)
    return true;
  return false;
}

/*****************************************************************************/
static bool canNalBeReordered(AL_ENut nut)
{
  switch(nut)
  {
  case AL_AVC_NUT_UNSPEC_0:
  case AL_AVC_NUT_SUB_SPS:
  case AL_AVC_NUT_DPS:
  case AL_AVC_NUT_RSV17:
  case AL_AVC_NUT_RSV18:
  case AL_AVC_NUT_RSV22:
  case AL_AVC_NUT_AUX:
  case AL_AVC_NUT_EXT:
  case AL_AVC_NUT_3D_EXT:
  case AL_AVC_NUT_RSV23:
  case AL_AVC_NUT_UNSPEC_24:
  case AL_AVC_NUT_UNSPEC_25:
  case AL_AVC_NUT_UNSPEC_26:
  case AL_AVC_NUT_UNSPEC_27:
  case AL_AVC_NUT_UNSPEC_28:
  case AL_AVC_NUT_UNSPEC_29:
  case AL_AVC_NUT_UNSPEC_30:
  case AL_AVC_NUT_UNSPEC_31:
    return true;
  default:
    return false;
  }
}

/*****************************************************************************/
void AL_AVC_InitParser(AL_NalParser* pParser)
{
  pParser->parseDps = NULL;
  pParser->parseVps = NULL;
  pParser->parseSps = parseAndApplySPS;
  pParser->parsePps = parsePPSandUpdateConcealment;
  pParser->parseAps = NULL;
  pParser->parsePh = NULL;
  pParser->parseSei = AL_AVC_ParseSEI;
  pParser->decodeSliceData = decodeSliceData;
  pParser->isSliceData = isSliceData;
  pParser->finishPendingRequest = finishPreviousFrame;
  pParser->getNonVclNuts = getNonVclNuts;
  pParser->isNutError = isNutError;
  pParser->canNalBeReordered = canNalBeReordered;
}

/*****************************************************************************/
void AL_AVC_InitAUP(AL_TAvcAup* pAUP)
{
  for(int i = 0; i < AL_AVC_MAX_PPS; ++i)
    pAUP->pPPS[i].bConceal = true;

  for(int i = 0; i < AL_AVC_MAX_SPS; ++i)
    pAUP->pSPS[i].bConceal = true;

  pAUP->ePictureType = AL_SLICE_I;
  pAUP->pActiveSPS = NULL;
}

/*****************************************************************************/
AL_ERR CreateAvcDecoder(AL_TDecoder** hDec, AL_IDecScheduler* pScheduler, AL_TAllocator* pAllocator, AL_TDecSettings* pSettings, AL_TDecCallBacks* pCB)
{
  pSettings->tStream.iMaxRef = Min(MAX_REF, pSettings->tStream.iMaxRef);
  AL_ERR errorCode = AL_CreateDefaultDecoder((AL_TDecoder**)hDec, pScheduler, pAllocator, pSettings, pCB);

  if(!AL_IS_ERROR_CODE(errorCode))
  {
    AL_TDecoder* pDec = *hDec;
    AL_TDecCtx* pCtx = &pDec->ctx;

    AL_AVC_InitParser(&pCtx->parser);
  }

  return errorCode;
}
