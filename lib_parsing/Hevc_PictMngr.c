// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/****************************************************************************
   -----------------------------------------------------------------------------
 **************************************************************************//*!
   \addtogroup lib_decode_hls
   @{
   \file
 *****************************************************************************/

#include "Hevc_PictMngr.h"
#include "lib_common/HevcUtils.h"
#include "lib_parsing/HevcParser.h"

/*****************************************************************************/
void AL_HEVC_PictMngr_UpdateRecInfo(AL_TPictMngrCtx* pCtx, AL_TCropInfo const* pCropInfo, AL_EPicStruct ePicStruct)
{
  AL_PictMngr_UpdateDisplayBufferCrop(pCtx, pCtx->uFrameID, pCropInfo);
  AL_PictMngr_UpdateDisplayBufferPicStruct(pCtx, pCtx->uFrameID, ePicStruct);
}

/*****************************************************************************/
bool AL_HEVC_PictMngr_GetBuffers(AL_TPictMngrCtx* pCtx, AL_TDecSliceParam const* pSP, TBuffer* pListVirtAddr, TBuffer* pListAddr, TBufferPOC* pPOC, TBufferMV* pMV, AL_TRecBuffers* pRecs)
{
  return AL_PictMngr_GetBuffers(pCtx, pSP, pListVirtAddr, pListAddr, pPOC, pMV, pRecs);
}

/*************************************************************************/
void AL_HEVC_PictMngr_ClearDPB(AL_TPictMngrCtx* pCtx, AL_THevcSps const* pSPS, bool bClearRef, bool bNoOutputPrior)
{
  AL_TDpb* pDpb = &pCtx->DPB;

  // pre decoding output process
  if(bClearRef)
  {
    if(bNoOutputPrior)
      AL_Dpb_ClearOutput(pDpb);
    AL_PictMngr_Flush(pCtx);
  }

  AL_Dpb_HEVC_Cleanup(pDpb, pSPS->SpsMaxLatency, pSPS->sps_max_num_reorder_pics[pSPS->sps_max_sub_layers_minus1]);
  uint8_t uNode = AL_Dpb_GetHeadPOC(pDpb);

  while(uNode != uEndOfList && AL_Dpb_GetPicCount(pDpb) >= (pSPS->sps_max_dec_pic_buffering_minus1[pSPS->sps_max_sub_layers_minus1] + 1))
  {
    if(AL_Dpb_GetOutputFlag(pDpb, uNode))
      AL_Dpb_Display(&pCtx->DPB, uNode);

    if(AL_Dpb_GetMarkingFlag(pDpb, uNode) == UNUSED_FOR_REF && (!AL_Dpb_GetOutputFlag(pDpb, uNode) || AL_HEVC_IsSLNR(pDpb->Nodes[uNode].eNUT)))
    {
      uint8_t uDelete = uNode;
      uNode = AL_Dpb_GetNextPOC(pDpb, uNode);

      AL_Dpb_Remove(pDpb, uDelete);
    }
    else
      uNode = AL_Dpb_GetNextPOC(pDpb, uNode);
  }

  // Compute DPB fullness
  uint8_t max_dec_pict_buffering = pSPS->sps_max_dec_pic_buffering_minus1[pSPS->sps_max_sub_layers_minus1] + 1;
  // The number of ref shall be less than max_dec_pict_buffering, but this condition is reverse here for concealment
  max_dec_pict_buffering = Max(max_dec_pict_buffering, AL_Dpb_GetRefCount(pDpb) + 1);
  // clip to Max supported Ref
  max_dec_pict_buffering = Min(max_dec_pict_buffering, AL_Dpb_GetNumRef(pDpb) + 1);

  // Remove Unused for reference if DBP is Full
  uNode = AL_Dpb_GetHeadPOC(pDpb);

  while(uNode != uEndOfList && AL_Dpb_GetPicCount(pDpb) >= max_dec_pict_buffering)
  {
    if(AL_Dpb_GetOutputFlag(pDpb, uNode))
      AL_Dpb_Display(&pCtx->DPB, uNode);

    if(AL_Dpb_GetMarkingFlag(pDpb, uNode) == UNUSED_FOR_REF && pDpb->Nodes[uNode].iFramePOC < pDpb->iLastDisplayedPOC)
    {
      uint8_t uDelete = uNode;
      uNode = AL_Dpb_GetNextPOC(pDpb, uNode);

      AL_Dpb_Remove(pDpb, uDelete);
    }
    else
      uNode = AL_Dpb_GetNextPOC(pDpb, uNode);
  }

  // Remove oldest POC if DBP is Full
  if(uNode == uEndOfList && AL_Dpb_GetPicCount(pDpb) >= max_dec_pict_buffering)
  {
    uint8_t uDelete = AL_Dpb_GetHeadPOC(pDpb);
    uint8_t uCurNode = uDelete;

    while(uCurNode != uEndOfList)
    {
      if(pDpb->Nodes[uCurNode].iFramePOC < pDpb->Nodes[uDelete].iFramePOC)
        uDelete = uCurNode;
      uCurNode = AL_Dpb_GetNextPOC(pDpb, uCurNode);
    }

    AL_Dpb_Remove(pDpb, uDelete);
  }
}

/*************************************************************************/
static bool IsShortOrLongTermRef(AL_EMarkingRef eMarking)
{
  return (eMarking == SHORT_TERM_REF) || (eMarking == LONG_TERM_REF);
}

/*************************************************************************/
bool AL_HEVC_PictMngr_HasPictInDPB(AL_TPictMngrCtx const* pCtx)
{
  AL_TDpb const* pDpb = &pCtx->DPB;
  uint8_t uNode = AL_Dpb_GetHeadPOC(pDpb);

  while(uNode != uEndOfList)
  {
    if(IsShortOrLongTermRef(AL_Dpb_GetMarkingFlag(pDpb, uNode)))
      return true;
    uNode = AL_Dpb_GetNextPOC(pDpb, uNode);
  }

  return false;
}

/*************************************************************************/
void AL_HEVC_PictMngr_RemoveHeadFrame(AL_TPictMngrCtx* pCtx)
{
  AL_TDpb* pDpb = &pCtx->DPB;

  if(AL_Dpb_GetPicCount(pDpb) >= AL_Dpb_GetNumRef(pDpb))
    AL_Dpb_RemoveHead(pDpb);
}

/*************************************************************************/
void AL_HEVC_PictMngr_EndFrame(AL_TPictMngrCtx* pCtx, uint32_t uPocLsb, AL_ENut eNUT, AL_THevcSliceHdr const* pSlice, uint8_t pic_output_flag)
{
  AL_TDpb* pDpb = &pCtx->DPB;

  AL_HEVC_PictMngr_RemoveHeadFrame(pCtx);

  // post decoding output process
  uint8_t uNode = AL_Dpb_GetHeadPOC(pDpb);

  if(pic_output_flag)
  {
    while(uNode != uEndOfList)
    {
      AL_Dpb_IncrementPicLatency(pDpb, uNode, pCtx->iCurFramePOC);
      uNode = AL_Dpb_GetNextPOC(pDpb, uNode);
    }
  }

  AL_PictMngr_Insert(pCtx, pCtx->iCurFramePOC, AL_PS_FRM, uPocLsb, pCtx->uFrameID, pCtx->uMvID, pic_output_flag, SHORT_TERM_REF, 0, eNUT, 0);
  AL_Dpb_HEVC_Cleanup(pDpb, pSlice->pSPS->SpsMaxLatency, pSlice->pSPS->sps_max_num_reorder_pics[pSlice->pSPS->sps_max_sub_layers_minus1]);
}

/*************************************************************************//*!
   \brief Prepares the reference picture set for the current slice reference picture list construction
   \param[in]  pCtx       Pointer to a Picture manager context object
   \param[in]  pSlice     Pointer to the slice header of the current slice
*****************************************************************************/
void AL_HEVC_PictMngr_InitRefPictSet(AL_TPictMngrCtx* pCtx, AL_THevcSliceHdr const* pSlice)
{
  uint8_t CurrDeltaPocMsbPresentFlag[16] = { 0 };
  uint8_t FollDeltaPocMsbPresentFlag[16] = { 0 };
  AL_TDpb* pDpb = &pCtx->DPB;

  // Fill the five lists of picture order count values
  if(!AL_HEVC_IsIDR(pSlice->nal_unit_type))
  {
    uint8_t i, j, k;
    AL_THevcSps* pSPS = pSlice->pSPS;
    uint8_t StRpsIdx = pSlice->short_term_ref_pic_set_sps_flag ? pSlice->short_term_ref_pic_set_idx :
                       pSPS->num_short_term_ref_pic_sets;

    // compute short term reference picture variables
    for(i = 0, j = 0, k = 0; i < pSPS->NumNegativePics[StRpsIdx]; ++i)
    {
      if(pSPS->UsedByCurrPicS0[StRpsIdx][i])
        pCtx->HevcRef.PocStCurrBefore[j++] = pCtx->iCurFramePOC + pSPS->DeltaPocS0[StRpsIdx][i];
      else
        pCtx->HevcRef.PocStFoll[k++] = pCtx->iCurFramePOC + pSPS->DeltaPocS0[StRpsIdx][i];
    }

    for(i = 0, j = 0; i < pSPS->NumPositivePics[StRpsIdx]; ++i)
    {
      if(pSPS->UsedByCurrPicS1[StRpsIdx][i])
        pCtx->HevcRef.PocStCurrAfter[j++] = pCtx->iCurFramePOC + pSPS->DeltaPocS1[StRpsIdx][i];
      else
        pCtx->HevcRef.PocStFoll[k++] = pCtx->iCurFramePOC + pSPS->DeltaPocS1[StRpsIdx][i];
    }

    // compute long term reference picture variables
    for(i = 0, j = 0, k = 0; i < pSlice->num_long_term_sps + pSlice->num_long_term_pics; ++i)
    {
      uint32_t uPocLt = pSlice->PocLsbLt[i];

      if(pSlice->delta_poc_msb_present_flag[i])
        uPocLt += pCtx->iCurFramePOC - (pSlice->DeltaPocMSBCycleLt[i] * pSPS->MaxPicOrderCntLsb) - pSlice->slice_pic_order_cnt_lsb;

      if(pSlice->UsedByCurrPicLt[i])
      {
        pCtx->HevcRef.PocLtCurr[j] = uPocLt;
        CurrDeltaPocMsbPresentFlag[j++] = pSlice->delta_poc_msb_present_flag[i];
      }
      else
      {
        pCtx->HevcRef.PocLtFoll[k] = uPocLt;
        FollDeltaPocMsbPresentFlag[k++] = pSlice->delta_poc_msb_present_flag[i];
      }
    }
  }

  // Compute long term reference pictures
  for(int i = 0; i < pSlice->NumPocLtCurr; ++i)
  {
    uint8_t uPos;

    if(!CurrDeltaPocMsbPresentFlag[i])
      uPos = AL_Dpb_SearchPocLsb(&pCtx->DPB, pCtx->HevcRef.PocLtCurr[i]);
    else
      uPos = AL_Dpb_SearchPOC(&pCtx->DPB, pCtx->HevcRef.PocLtCurr[i]);
    pCtx->HevcRef.RefPicSetLtCurr[i] = uPos;
  }

  for(int i = 0; i < pSlice->NumPocLtFoll; ++i)
  {
    uint8_t uPos;

    if(!FollDeltaPocMsbPresentFlag[i])
      uPos = AL_Dpb_SearchPocLsb(&pCtx->DPB, pCtx->HevcRef.PocLtFoll[i]);
    else
      uPos = AL_Dpb_SearchPOC(&pCtx->DPB, pCtx->HevcRef.PocLtFoll[i]);
    pCtx->HevcRef.RefPicSetLtFoll[i] = uPos;
  }

  // Compute short term reference pictures
  for(int i = 0; i < pSlice->NumPocStCurrBefore; ++i)
    pCtx->HevcRef.RefPicSetStCurrBefore[i] = AL_Dpb_SearchPOC(&pCtx->DPB, pCtx->HevcRef.PocStCurrBefore[i]);

  for(int i = 0; i < pSlice->NumPocStCurrAfter; ++i)
    pCtx->HevcRef.RefPicSetStCurrAfter[i] = AL_Dpb_SearchPOC(&pCtx->DPB, pCtx->HevcRef.PocStCurrAfter[i]);

  for(int i = 0; i < pSlice->NumPocStFoll; ++i)
    pCtx->HevcRef.RefPicSetStFoll[i] = AL_Dpb_SearchPOC(&pCtx->DPB, pCtx->HevcRef.PocStFoll[i]);

  int iNumRefAfterUpdate = pSlice->NumPocLtCurr
                           + pSlice->NumPocLtFoll
                           + pSlice->NumPocStCurrBefore
                           + pSlice->NumPocStCurrAfter
                           + pSlice->NumPocStFoll;

  // Error Concealment : do not change anything if there is no reference after RPS update
  if(pSlice->slice_type != AL_SLICE_I && iNumRefAfterUpdate == 0)
    return;

  // reset picture marking on all the picture in the dbp
  uint8_t uNode = AL_Dpb_GetHeadPOC(&pCtx->DPB);

  while(uNode != uEndOfList)
  {
    AL_Dpb_SetMarkingFlag(pDpb, uNode, UNUSED_FOR_REF);
    uNode = AL_Dpb_GetNextPOC(pDpb, uNode);
  }

  // mark long term reference pictures
  for(int i = 0; i < pSlice->NumPocLtCurr; ++i)
  {
    uNode = pCtx->HevcRef.RefPicSetLtCurr[i];

    if(uNode != uEndOfList)
      AL_Dpb_SetMarkingFlag(pDpb, uNode, LONG_TERM_REF);
  }

  for(int i = 0; i < pSlice->NumPocLtFoll; ++i)
  {
    uNode = pCtx->HevcRef.RefPicSetLtFoll[i];

    if(uNode != uEndOfList)
      AL_Dpb_SetMarkingFlag(pDpb, uNode, LONG_TERM_REF);
  }

  // mark short term reference pictures
  for(int i = 0; i < pSlice->NumPocStCurrBefore; ++i)
  {
    uNode = pCtx->HevcRef.RefPicSetStCurrBefore[i];

    if(uNode != uEndOfList)
      AL_Dpb_SetMarkingFlag(pDpb, uNode, SHORT_TERM_REF);
  }

  for(int i = 0; i < pSlice->NumPocStCurrAfter; ++i)
  {
    uNode = pCtx->HevcRef.RefPicSetStCurrAfter[i];

    if(uNode != uEndOfList)
      AL_Dpb_SetMarkingFlag(pDpb, uNode, SHORT_TERM_REF);
  }

  for(int i = 0; i < pSlice->NumPocStFoll; ++i)
  {
    uNode = pCtx->HevcRef.RefPicSetStFoll[i];

    if(uNode != uEndOfList)
      AL_Dpb_SetMarkingFlag(pDpb, uNode, SHORT_TERM_REF);
  }
}

/*************************************************************************//*!
   \brief Builds the reference picture list of the current slice
   \param[in]  pCtx     Pointer to a Picture manager context object
   \param[in]  pSlice   Pointer to the slice header of the current slice
   \param[out] pListRef Pointer to the current reference list
*****************************************************************************/
bool AL_HEVC_PictMngr_BuildPictureList(AL_TPictMngrCtx* pCtx, AL_THevcSliceHdr const* pSlice, TBufferListRef* pListRef)
{
  uint8_t uRef;
  uint8_t pNumRef[2] =
  {
    0, 0
  };
  uint8_t NumPocTotalCurr = pSlice->NumPocTotalCurr;

  // reset reference picture list
  for(uRef = 0; uRef < MAX_REF; ++uRef)
  {
    (*pListRef)[0][uRef].uNodeID = uEndOfList;
    (*pListRef)[1][uRef].uNodeID = uEndOfList;
  }

  if(pSlice->slice_type != AL_SLICE_I)
  {
    uint8_t uNodeList[16];
    uint8_t NumRpsCurrTempList = (NumPocTotalCurr > pSlice->num_ref_idx_l0_active_minus1 + 1) ? NumPocTotalCurr : pSlice->num_ref_idx_l0_active_minus1 + 1;
    // slice P
    uRef = 0;

    if(pSlice->NumPocStCurrBefore || pSlice->NumPocStCurrAfter || pSlice->NumPocLtCurr)
    {
      while(uRef < NumRpsCurrTempList)
      {
        for(uint8_t i = 0; i < pSlice->NumPocStCurrBefore && uRef < NumRpsCurrTempList; ++uRef, ++i)
          uNodeList[uRef] = pCtx->HevcRef.RefPicSetStCurrBefore[i];

        for(uint8_t i = 0; i < pSlice->NumPocStCurrAfter && uRef < NumRpsCurrTempList; ++uRef, ++i)
          uNodeList[uRef] = pCtx->HevcRef.RefPicSetStCurrAfter[i];

        for(uint8_t i = 0; i < pSlice->NumPocLtCurr && uRef < NumRpsCurrTempList; ++uRef, ++i)
          uNodeList[uRef] = pCtx->HevcRef.RefPicSetLtCurr[i];
      }

      for(uRef = 0; uRef <= pSlice->num_ref_idx_l0_active_minus1; ++uRef)
      {
        uint8_t uNode = pSlice->ref_pic_modif.ref_pic_list_modification_flag_l0 ? uNodeList[pSlice->ref_pic_modif.list_entry_l0[uRef]] :
                        uNodeList[uRef];

        if((uNode == uEndOfList) || (pCtx->DPB.Nodes[uNode].uFrmID == UndefID))
          uNode = AL_Dpb_GetHeadPOC(&pCtx->DPB);

        if((uNode == uEndOfList) || (pCtx->DPB.Nodes[uNode].uFrmID == UndefID))
          return false;

        (*pListRef)[0][uRef].uNodeID = uNode;
        (*pListRef)[0][uRef].RefBuf = *(AL_PictMngr_GetRecBufferFromID(pCtx, pCtx->DPB.Nodes[uNode].uFrmID));
      }
    }

    // slice B
    if(pSlice->slice_type == AL_SLICE_B)
    {
      NumRpsCurrTempList = (NumPocTotalCurr > pSlice->num_ref_idx_l1_active_minus1 + 1) ? NumPocTotalCurr : pSlice->num_ref_idx_l1_active_minus1 + 1;
      uRef = 0;

      if(pSlice->NumPocStCurrAfter || pSlice->NumPocStCurrBefore || pSlice->NumPocLtCurr)
      {
        while(uRef < NumRpsCurrTempList)
        {
          for(uint8_t i = 0; i < pSlice->NumPocStCurrAfter && uRef < NumRpsCurrTempList; ++uRef, ++i)
            uNodeList[uRef] = pCtx->HevcRef.RefPicSetStCurrAfter[i];

          for(uint8_t i = 0; i < pSlice->NumPocStCurrBefore && uRef < NumRpsCurrTempList; ++uRef, ++i)
            uNodeList[uRef] = pCtx->HevcRef.RefPicSetStCurrBefore[i];

          for(uint8_t i = 0; i < pSlice->NumPocLtCurr && uRef < NumRpsCurrTempList; ++uRef, ++i)
            uNodeList[uRef] = pCtx->HevcRef.RefPicSetLtCurr[i];
        }

        for(uRef = 0; uRef <= pSlice->num_ref_idx_l1_active_minus1; ++uRef)
        {
          uint8_t uNode = pSlice->ref_pic_modif.ref_pic_list_modification_flag_l1 ? uNodeList[pSlice->ref_pic_modif.list_entry_l1[uRef]] :
                          uNodeList[uRef];

          if((uNode == uEndOfList) || (pCtx->DPB.Nodes[uNode].uFrmID == UndefID))
            uNode = AL_Dpb_GetHeadPOC(&pCtx->DPB);

          if((uNode == uEndOfList) || (pCtx->DPB.Nodes[uNode].uFrmID == UndefID))
            return false;

          (*pListRef)[1][uRef].uNodeID = uNode;
          (*pListRef)[1][uRef].RefBuf = *(AL_PictMngr_GetRecBufferFromID(pCtx, pCtx->DPB.Nodes[uNode].uFrmID));
        }
      }
    }
  }

  for(uint8_t i = 0; i < 16; ++i)
  {
    if((*pListRef)[0][i].uNodeID != uEndOfList)
      pNumRef[0]++;

    if((*pListRef)[1][i].uNodeID != uEndOfList)
      pNumRef[1]++;
  }

  if((pSlice->slice_type != AL_SLICE_I && pNumRef[0] < pSlice->num_ref_idx_l0_active_minus1 + 1) ||
     (pSlice->slice_type == AL_SLICE_B && pNumRef[1] < pSlice->num_ref_idx_l1_active_minus1 + 1))
    return false;

  return true;
}

/*@}*/
