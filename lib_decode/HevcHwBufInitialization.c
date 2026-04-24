// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "HevcHwBufInitialization.h"
#include "lib_common_dec/DecBuffersInternal.h"
#include "lib_common_dec/DecHwScalingList.h"

/*****************************************************************************/
void AL_HEVC_InitHWFrameBuffers(AL_TScl const* pSclLst, AL_TDecPicBuffers* pBufs)
{
  AL_HEVC_WriteDecHwScalingList(pSclLst, pBufs->tScl.tMD.pVirtualAddr);
}

/*****************************************************************************/
static void AL_HEVC_FillWeightedPredCoeff(AL_VADDR pDataWP, AL_THevcSliceHdr const* pSlice, uint8_t uL0L1)
{
  uint8_t uNumRefIdx = (uL0L1 ? pSlice->num_ref_idx_l1_active_minus1 : pSlice->num_ref_idx_l0_active_minus1) + 1;
  uint32_t* pWP = (uint32_t*)(pDataWP + uL0L1 * WP_ONE_SET_SIZE);

  AL_TWPCoeff const* pWpCoeff = &pSlice->pred_weight_table.tWpCoeff[uL0L1];

  for(uint8_t i = 0; i < uNumRefIdx; ++i)
  {
    pWP[0] = ((pWpCoeff->luma_offset[i] & 0x3FF)) |
             ((pWpCoeff->chroma_offset[i][0] & 0x3FF) << 10) |
             ((pWpCoeff->chroma_offset[i][1] & 0x3FF) << 20);

    pWP[1] = ((pWpCoeff->luma_delta_weight[i] & 0xFF)) |
             ((pWpCoeff->chroma_delta_weight[i][0] & 0xFF) << 8) |
             ((pWpCoeff->chroma_delta_weight[i][1] & 0xFF) << 16) |
             ((pSlice->pred_weight_table.luma_log2_weight_denom & 0x0F) << 24) |
             ((pSlice->pred_weight_table.chroma_log2_weight_denom & 0x0F) << 28);
    pWP += 2 * WP_ONE_SET_SIZE / 4;
  }
}

/*****************************************************************************/
static void AL_HEVC_WriteWeightedPredCoeff(uint16_t uSliceIndex, AL_THevcSliceHdr const* pSlice, TBuffer* pWP)
{
  AL_VADDR pDataWP = pWP->tMD.pVirtualAddr + (uSliceIndex * WP_SLICE_SIZE);

  // weighted pred case
  if((pSlice->pPPS->weighted_bipred_flag && pSlice->slice_type == AL_SLICE_B) ||
     (pSlice->pPPS->weighted_pred_flag && pSlice->slice_type == AL_SLICE_P))
  {
    Rtos_Memset(pDataWP, 0, WP_SLICE_SIZE);

    AL_HEVC_FillWeightedPredCoeff(pDataWP, pSlice, 0);

    if(pSlice->slice_type == AL_SLICE_B)
      AL_HEVC_FillWeightedPredCoeff(pDataWP, pSlice, 1);
  }
}

/*****************************************************************************/
void AL_HEVC_InitHWSliceBuffers(uint16_t uSliceIndex, AL_THevcSliceHdr const* pSlice, AL_TDecPicBuffers* pBufs)
{
  AL_HEVC_WriteWeightedPredCoeff(uSliceIndex, pSlice, &pBufs->tWP);
}
