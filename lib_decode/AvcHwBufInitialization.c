// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "AvcHwBufInitialization.h"
#include "lib_common_dec/DecBuffersInternal.h"
#include "lib_common_dec/DecHwScalingList.h"
#include "lib_common/PicFormat.h"
#include "lib_rtos/lib_rtos.h"

/*****************************************************************************/
void AL_AVC_InitHWFrameBuffers(AL_TScl const* pSclLst, AL_EChromaMode eCMode, AL_TDecPicBuffers* pBufs)
{
  AL_AVC_WriteDecHwScalingList(pSclLst, eCMode, pBufs->tScl.tMD.pVirtualAddr);
}

/*****************************************************************************/
static void AL_AVC_FillWeightedPredCoeff(AL_VADDR pDataWP, AL_TAvcSliceHdr const* pSlice, uint8_t uL0L1)
{
  uint8_t uNumRefIdx = (uL0L1 ? pSlice->num_ref_idx_l1_active_minus1 : pSlice->num_ref_idx_l0_active_minus1) + 1;
  uint32_t* pWP = (uint32_t*)(pDataWP + (uL0L1 * WP_ONE_SET_SIZE));

  AL_TWPCoeff const* pWpCoeff = &pSlice->pred_weight_table.tWpCoeff[uL0L1];

  for(int i = 0; i < uNumRefIdx; ++i)
  {
    pWP[0] = ((pWpCoeff->luma_offset[i] & 0x3FF)) |
             ((pWpCoeff->chroma_offset[i][0] & 0x3FF) << 10) |
             ((pWpCoeff->chroma_offset[i][1] & 0x3FF) << 20);

    pWP[1] = ((pWpCoeff->luma_delta_weight[i] & 0xFF)) |
             ((pWpCoeff->chroma_delta_weight[i][0] & 0xFF) << 8) |
             ((pWpCoeff->chroma_delta_weight[i][1] & 0xFF) << 16) |
             ((pSlice->pred_weight_table.luma_log2_weight_denom & 0x07) << 24) |
             ((pWpCoeff->luma_weight_flag[i] & 0x01) << 27) |
             ((pSlice->pred_weight_table.chroma_log2_weight_denom & 0x07) << 28) |
             ((pWpCoeff->chroma_weight_flag[i] & 0x01u) << 31);

    pWP += 2 * WP_ONE_SET_SIZE / 4;
  }
}

/*****************************************************************************/
static void AL_AVC_WriteWeightedPredCoeff(uint16_t uSliceIndex, AL_TAvcSliceHdr const* pSlice, TBuffer* pWP)
{
  AL_VADDR pDataWP = pWP->tMD.pVirtualAddr + (uSliceIndex * WP_SLICE_SIZE);
  Rtos_Memset(pDataWP, 0, WP_SLICE_SIZE);

  // explicit weighted_pred case
  if((pSlice->slice_type == AL_SLICE_P && pSlice->pPPS->weighted_pred_flag) ||
     (pSlice->slice_type == AL_SLICE_B && pSlice->pPPS->weighted_bipred_idc == 1))
  {
    AL_AVC_FillWeightedPredCoeff(pDataWP, pSlice, 0);

    if(pSlice->slice_type == AL_SLICE_B)
      AL_AVC_FillWeightedPredCoeff(pDataWP, pSlice, 1);
  }
}

/*****************************************************************************/
void AL_AVC_InitHWSliceBuffers(uint16_t uSliceIndex, AL_TAvcSliceHdr const* pSlice, AL_TDecPicBuffers* pBufs)
{
  AL_AVC_WriteWeightedPredCoeff(uSliceIndex, pSlice, &pBufs->tWP);
}
