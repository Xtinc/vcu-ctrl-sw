// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "AvcParser.h"
#include "lib_rtos/lib_rtos.h"
#include "lib_common/Utils.h"
#include "lib_common/SeiInternal.h"
#include "lib_common_dec/RbspParser.h"
#include "SeiParser.h"

static void initPps(AL_TAvcPps* pPPS)
{
  Rtos_Memset(pPPS->UseDefaultScalingMatrix4x4Flag, 0, sizeof(pPPS->UseDefaultScalingMatrix4x4Flag));
  Rtos_Memset(pPPS->UseDefaultScalingMatrix8x8Flag, 0, sizeof(pPPS->UseDefaultScalingMatrix8x8Flag));
  pPPS->transform_8x8_mode_flag = 0;
  pPPS->pic_scaling_matrix_present_flag = 0;
  pPPS->bConceal = true;
}

static void propagate_scaling_list(int iSclID, uint8_t* Default4x4Flag, uint8_t* Default8x8Flag, uint8_t ScalingList4x4[][16], uint8_t ScalingList8x8[][64])
{
  if(iSclID < 6)
  {
    if(iSclID == 0 || iSclID == 3)
      Default4x4Flag[iSclID] = 1;
    else
    {
      if(Default4x4Flag[iSclID - 1])
        Default4x4Flag[iSclID] = 1;
      else
        Rtos_Memcpy(ScalingList4x4[iSclID], ScalingList4x4[iSclID - 1], 16);
    }
  }
  else
  {
    if(iSclID < 8)
      Default8x8Flag[iSclID - 6] = 1;
    else
    {
      if(Default8x8Flag[iSclID - 8])
        Default8x8Flag[iSclID - 6] = 1;
      else
        Rtos_Memcpy(ScalingList8x8[iSclID - 6], ScalingList8x8[iSclID - 8], 64);
    }
  }
}

AL_PARSE_RESULT AL_AVC_ParsePPS(AL_TAup* pIAup, AL_TRbspParser* pRP, uint16_t* pPpsId)
{
  skipAllZerosAndTheNextByte(pRP);
  u(pRP, 8); // Skip NUT

  uint16_t pps_id = ue(pRP);

  if(pPpsId)
    *pPpsId = pps_id;

  COMPLY(pps_id < AL_AVC_MAX_PPS);

  AL_TAvcAup* aup = &pIAup->avcAup;
  AL_TAvcPps* pPPS = &aup->pPPS[pps_id];

  // default values
  initPps(pPPS);

  pPPS->pic_parameter_set_id = pps_id;
  pPPS->seq_parameter_set_id = ue(pRP);

  COMPLY(pPPS->seq_parameter_set_id < AL_AVC_MAX_SPS);

  pPPS->pSPS = &pIAup->avcAup.pSPS[pPPS->seq_parameter_set_id];

  COMPLY(!pPPS->pSPS->bConceal);

  pPPS->entropy_coding_mode_flag = u(pRP, 1);
  pPPS->bottom_field_pic_order_in_frame_present_flag = u(pRP, 1);
  pPPS->num_slice_groups_minus1 = Clip3(ue(pRP), 0, 7);

  if(pPPS->num_slice_groups_minus1 > 0)
  {
    pPPS->slice_group_map_type = ue(pRP);

    if(pPPS->slice_group_map_type == 0)
    {
      for(int iGroup = 0; iGroup <= pPPS->num_slice_groups_minus1; iGroup++)
        pPPS->run_length_minus1[iGroup] = ue(pRP);
    }
    else if(pPPS->slice_group_map_type == 2)
    {
      for(int iGroup = 0; iGroup <= pPPS->num_slice_groups_minus1; iGroup++)
      {
        pPPS->top_left[iGroup] = ue(pRP);
        pPPS->bottom_right[iGroup] = ue(pRP);
      }
    }
    else if(pPPS->slice_group_map_type == 3 ||
            pPPS->slice_group_map_type == 4 ||
            pPPS->slice_group_map_type == 5)
    {
      pPPS->slice_group_change_direction_flag = u(pRP, 1);
      pPPS->slice_group_change_rate_minus1 = ue(pRP);
    }
    else if(pPPS->slice_group_map_type == 6)
    {
      pPPS->pic_size_in_map_units_minus1 = Clip3(ue(pRP), 0, 8159);

      for(int i = 0; i <= pPPS->pic_size_in_map_units_minus1; i++)
      {
        int slicegroupsize = pPPS->pic_size_in_map_units_minus1 + 1;
        pPPS->slice_group_id[i] = u(pRP, slicegroupsize);
      }
    }
  }

  pPPS->num_ref_idx_l0_active_minus1 = Clip3(ue(pRP), 0, AL_AVC_MAX_REF_IDX);
  pPPS->num_ref_idx_l1_active_minus1 = Clip3(ue(pRP), 0, AL_AVC_MAX_REF_IDX);

  pPPS->weighted_pred_flag = u(pRP, 1);
  pPPS->weighted_bipred_idc = Clip3(u(pRP, 2), 0, AL_MAX_WP_IDC);

  uint16_t QpBdOffset = 6 * pPPS->pSPS->bit_depth_luma_minus8;
  pPPS->pic_init_qp_minus26 = Clip3(se(pRP), -(26 + QpBdOffset), AL_MAX_INIT_QP);
  pPPS->pic_init_qs_minus26 = Clip3(se(pRP), AL_MIN_INIT_QP, AL_MAX_INIT_QP);

  pPPS->chroma_qp_index_offset = Clip3(se(pRP), AL_MIN_QP_OFFSET, AL_MAX_QP_OFFSET);
  pPPS->second_chroma_qp_index_offset = pPPS->chroma_qp_index_offset;

  pPPS->deblocking_filter_control_present_flag = u(pRP, 1);
  pPPS->constrained_intra_pred_flag = u(pRP, 1);
  pPPS->redundant_pic_cnt_present_flag = u(pRP, 1);

  if(more_rbsp_data(pRP))
  {
    pPPS->transform_8x8_mode_flag = u(pRP, 1);
    pPPS->pic_scaling_matrix_present_flag = u(pRP, 1);

    if(pPPS->pic_scaling_matrix_present_flag)
    {
      for(int i = 0; i < 6 + (pPPS->pSPS->chroma_format_idc != 3 ? 2 : 6) * pPPS->transform_8x8_mode_flag; i++)
      {
        if(i < 6)
          pPPS->UseDefaultScalingMatrix4x4Flag[i] = 0;
        else
          pPPS->UseDefaultScalingMatrix8x8Flag[i - 6] = 0;

        pPPS->pic_scaling_list_present_flag[i] = u(pRP, 1);

        if(pPPS->pic_scaling_list_present_flag[i])
        {
          if(i < 6)
            avc_scaling_list_data(pPPS->ScalingList4x4[i], pRP, 16, &pPPS->UseDefaultScalingMatrix4x4Flag[i]);
          else
            avc_scaling_list_data(pPPS->ScalingList8x8[i - 6], pRP, 64, &pPPS->UseDefaultScalingMatrix8x8Flag[i - 6]);
        }
        else
        {
          if(!pPPS->pSPS->seq_scaling_matrix_present_flag)
            propagate_scaling_list(i, pPPS->UseDefaultScalingMatrix4x4Flag, pPPS->UseDefaultScalingMatrix8x8Flag, pPPS->ScalingList4x4, pPPS->ScalingList8x8);
          else
          {
            if(i < 6)
            {
              if(i == 0 || i == 3)
              {
                if(pPPS->pSPS->UseDefaultScalingMatrix4x4Flag[i])
                  pPPS->UseDefaultScalingMatrix4x4Flag[i] = 1;
                else
                  Rtos_Memcpy(pPPS->ScalingList4x4[i], pPPS->pSPS->ScalingList4x4[i], 16);
              }
              else
              {
                if(pPPS->UseDefaultScalingMatrix4x4Flag[i - 1])
                  pPPS->UseDefaultScalingMatrix4x4Flag[i] = 1;
                else
                  Rtos_Memcpy(pPPS->ScalingList4x4[i], pPPS->ScalingList4x4[i - 1], 16);
              }
            }
            else
            {
              if(i < 8)
              {
                if(pPPS->pSPS->UseDefaultScalingMatrix8x8Flag[i - 6])
                  pPPS->UseDefaultScalingMatrix8x8Flag[i - 6] = 1;
                else
                  Rtos_Memcpy(pPPS->ScalingList8x8[i - 6], pPPS->pSPS->ScalingList8x8[i - 6], 64);
              }
              else
              {
                if(pPPS->UseDefaultScalingMatrix8x8Flag[i - 8])
                  pPPS->UseDefaultScalingMatrix8x8Flag[i - 6] = 1;
                else
                  Rtos_Memcpy(pPPS->ScalingList8x8[i - 6], pPPS->ScalingList8x8[i - 8], 64);
              }
            }
          }
        }
      }
    }
    else if(pPPS->pSPS)
    {
      for(int i = 0; i < (pPPS->pSPS->chroma_format_idc != 3 ? 8 : 12); ++i)
      {
        if(i < 6)
        {
          if(pPPS->pSPS->UseDefaultScalingMatrix4x4Flag[i])
            pPPS->UseDefaultScalingMatrix4x4Flag[i] = 1;
          else
            Rtos_Memcpy(pPPS->ScalingList4x4[i], pPPS->pSPS->ScalingList4x4[i], 16);
        }
        else
        {
          if(pPPS->pSPS->UseDefaultScalingMatrix8x8Flag[i - 6])
            pPPS->UseDefaultScalingMatrix8x8Flag[i - 6] = 1;
          else
            Rtos_Memcpy(pPPS->ScalingList8x8[i - 6], pPPS->pSPS->ScalingList8x8[i - 6], 64);
        }
      }
    }

    pPPS->second_chroma_qp_index_offset = Clip3(se(pRP), AL_MIN_INIT_QP, AL_MAX_INIT_QP);
  }
  else
  {
    for(int i = 0; i < (pPPS->pSPS->chroma_format_idc != 3 ? 8 : 12); ++i)
    {
      if(i < 6)
      {
        if(pPPS->pSPS->UseDefaultScalingMatrix4x4Flag[i])
          pPPS->UseDefaultScalingMatrix4x4Flag[i] = 1;
        else
          Rtos_Memcpy(pPPS->ScalingList4x4[i], pPPS->pSPS->ScalingList4x4[i], 16);
      }
      else
      {
        if(pPPS->pSPS->UseDefaultScalingMatrix8x8Flag[i - 6])
          pPPS->UseDefaultScalingMatrix8x8Flag[i - 6] = 1;
        else
          Rtos_Memcpy(pPPS->ScalingList8x8[i - 6], pPPS->pSPS->ScalingList8x8[i - 6], 64);
      }
    }
  }

  /*dummy information to ensure there's no zero value in scaling list structure (div by zero prevention)*/
  if(!pPPS->transform_8x8_mode_flag)
  {
    for(int i = 0; i < 6; ++i)
      pPPS->UseDefaultScalingMatrix8x8Flag[i] = 1;
  }

  COMPLY(rbsp_trailing_bits(pRP));

  return AL_OK;
}

static void initSps(AL_TAvcSps* pSPS)
{
  Rtos_Memset(pSPS, 0, sizeof(AL_TAvcSps));

  pSPS->vui_param.hrd_param.initial_cpb_removal_delay_length_minus1 = 23;
  pSPS->vui_param.hrd_param.au_cpb_removal_delay_length_minus1 = 23;
  pSPS->vui_param.hrd_param.dpb_output_delay_length_minus1 = 23;

  pSPS->mb_adaptive_frame_field_flag = 0;
  pSPS->chroma_format_idc = 1;
  pSPS->bit_depth_chroma_minus8 = 0;
  pSPS->bit_depth_luma_minus8 = 0;
  pSPS->seq_scaling_matrix_present_flag = 0;
  pSPS->qpprime_y_zero_transform_bypass_flag = 0;
  pSPS->vui_param.max_bytes_per_pic_denom = 2;
  pSPS->vui_param.max_bits_per_min_cu_denom = 1;
  pSPS->vui_param.log2_max_mv_length_horizontal = 16;
  pSPS->vui_param.log2_max_mv_length_vertical = 16;

  pSPS->bConceal = true;
}

static bool isProfileSupported(uint8_t profile_idc)
{
  switch(profile_idc)
  {
  case AVC_PROFILE_IDC_BASELINE: // We do not support FMO / ASO, however, we try to decode with concealment
    return true;

  case AVC_PROFILE_IDC_MAIN:
  case AVC_PROFILE_IDC_HIGH:
    return true;

  case AVC_PROFILE_IDC_HIGH_422:
  case AVC_PROFILE_IDC_HIGH10:
    return true;

  default:
    return false;
  }
}

/*****************************************************************************/
AL_PARSE_RESULT AL_AVC_ParseSPS(AL_TRbspParser* pRP, AL_TAvcSps* pSPS)
{
  skipAllZerosAndTheNextByte(pRP);
  u(pRP, 8); // Skip NUT

  initSps(pSPS);

  pSPS->profile_idc = u(pRP, 8);
  pSPS->constraint_set0_flag = u(pRP, 1);
  pSPS->constraint_set1_flag = u(pRP, 1);
  pSPS->constraint_set2_flag = u(pRP, 1);
  pSPS->constraint_set3_flag = u(pRP, 1);
  pSPS->constraint_set4_flag = u(pRP, 1);
  pSPS->constraint_set5_flag = u(pRP, 1);
  skip(pRP, 2); // reserved_zero_bits

  pSPS->level_idc = u(pRP, 8);

  pSPS->seq_parameter_set_id = ue(pRP);
  COMPLY_ID(pSPS->seq_parameter_set_id < AL_AVC_MAX_SPS);

  if(!isProfileSupported(pSPS->profile_idc) && !pSPS->constraint_set1_flag)
  {
    VIDEO_ERROR_PRINT("Profile '%i' is not supported.", pSPS->profile_idc);
    return AL_UNSUPPORTED;
  }

  if(pSPS->profile_idc == 44 || pSPS->profile_idc == 83 ||
     pSPS->profile_idc == 86 || pSPS->profile_idc == 100 ||
     pSPS->profile_idc == 110 || pSPS->profile_idc == 118 ||
     pSPS->profile_idc == 122 || pSPS->profile_idc == 128 ||
     pSPS->profile_idc == 244)
  {
    pSPS->chroma_format_idc = ue(pRP);

    COMPLY(pSPS->chroma_format_idc < AL_CHROMA_MAX_ENUM);

    if(pSPS->chroma_format_idc == 3)
      pSPS->separate_colour_plane_flag = u(pRP, 1);
    pSPS->bit_depth_luma_minus8 = Clip3(ue(pRP), 0, MAX_BIT_DEPTH);
    pSPS->bit_depth_chroma_minus8 = Clip3(ue(pRP), 0, MAX_BIT_DEPTH);

    pSPS->qpprime_y_zero_transform_bypass_flag = u(pRP, 1);
    pSPS->seq_scaling_matrix_present_flag = u(pRP, 1);

    if(pSPS->seq_scaling_matrix_present_flag)
    {
      for(int i = 0; i < (pSPS->chroma_format_idc != 3 ? 8 : 12); ++i)
      {
        pSPS->seq_scaling_list_present_flag[i] = u(pRP, 1);

        if(pSPS->seq_scaling_list_present_flag[i])
        {
          if(i < 6)
            avc_scaling_list_data(pSPS->ScalingList4x4[i], pRP, 16, &pSPS->UseDefaultScalingMatrix4x4Flag[i]);
          else
            avc_scaling_list_data(pSPS->ScalingList8x8[i - 6], pRP, 64, &pSPS->UseDefaultScalingMatrix8x8Flag[i - 6]);
        }
        else
          propagate_scaling_list(i, pSPS->UseDefaultScalingMatrix4x4Flag, pSPS->UseDefaultScalingMatrix8x8Flag, pSPS->ScalingList4x4, pSPS->ScalingList8x8);
      }
    }
    else
    {
      for(int i = 0; i < (pSPS->chroma_format_idc != 3 ? 8 : 12); ++i)
      {
        if(i < 6)
          Rtos_Memset(pSPS->ScalingList4x4[i], 16, 16);
        else
          Rtos_Memset(pSPS->ScalingList8x8[i - 6], 16, 64);
      }
    }
  }

  pSPS->log2_max_frame_num_minus4 = ue(pRP);

  COMPLY(pSPS->log2_max_frame_num_minus4 <= MAX_FRAME_NUM);

  pSPS->pic_order_cnt_type = ue(pRP);

  COMPLY(pSPS->pic_order_cnt_type <= MAX_POC_TYPE);

  if(pSPS->pic_order_cnt_type == 0)
  {
    pSPS->log2_max_pic_order_cnt_lsb_minus4 = ue(pRP);

    COMPLY(pSPS->log2_max_pic_order_cnt_lsb_minus4 <= MAX_POC_LSB);

    pSPS->delta_pic_order_always_zero_flag = 1;
  }
  else if(pSPS->pic_order_cnt_type == 1)
  {
    pSPS->delta_pic_order_always_zero_flag = u(pRP, 1);
    pSPS->offset_for_non_ref_pic = se(pRP);
    pSPS->offset_for_top_to_bottom_field = se(pRP);
    pSPS->num_ref_frames_in_pic_order_cnt_cycle = ue(pRP);

    for(int i = 0; i < pSPS->num_ref_frames_in_pic_order_cnt_cycle; i++)
      pSPS->offset_for_ref_frame[i] = se(pRP);
  }

  pSPS->max_num_ref_frames = Clip3(ue(pRP), 0, MAX_REF);
  pSPS->gaps_in_frame_num_value_allowed_flag = u(pRP, 1);

  pSPS->pic_width_in_mbs_minus1 = ue(pRP);
  pSPS->pic_height_in_map_units_minus1 = ue(pRP);

  COMPLY(pSPS->pic_width_in_mbs_minus1 >= 1);
  COMPLY(pSPS->pic_height_in_map_units_minus1 >= 1);

  pSPS->frame_mbs_only_flag = u(pRP, 1);

  if(!pSPS->frame_mbs_only_flag)
  {
    pSPS->mb_adaptive_frame_field_flag = u(pRP, 1);

    if(pSPS->mb_adaptive_frame_field_flag)
    {
      VIDEO_ERROR_PRINT("MBAFF is not supported");
      return AL_UNSUPPORTED;
    }
  }

  pSPS->direct_8x8_inference_flag = u(pRP, 1);
  pSPS->frame_cropping_flag = u(pRP, 1);

  if(pSPS->frame_cropping_flag)
  {
    pSPS->frame_crop_left_offset = ue(pRP);
    pSPS->frame_crop_right_offset = ue(pRP);
    pSPS->frame_crop_top_offset = ue(pRP);
    pSPS->frame_crop_bottom_offset = ue(pRP);
  }

  pSPS->vui_parameters_present_flag = u(pRP, 1);

  if(pSPS->vui_parameters_present_flag)
    COMPLY(avc_vui_parameters(&pSPS->vui_param, pRP));

  // validate current SPS
  pSPS->bConceal = false;

  return AL_OK;
}

/*****************************************************************************/
static bool SeiBufferingPeriod(AL_TRbspParser* pRP, AL_TAvcSps* pSpsTable, AL_TAvcBufPeriod* pBufPeriod, AL_TAvcSps** pActiveSps)
{
  AL_TAvcSps* pSPS = NULL;

  pBufPeriod->seq_parameter_set_id = ue(pRP);

  if(pBufPeriod->seq_parameter_set_id >= AL_AVC_MAX_SPS)
    return false;

  pSPS = &pSpsTable[pBufPeriod->seq_parameter_set_id];

  if(pSPS->bConceal)
    return false;

  *pActiveSps = pSPS;

  if(pSPS->vui_param.hrd_param.nal_hrd_parameters_present_flag)
  {
    uint8_t syntax_size = pSPS->vui_param.hrd_param.initial_cpb_removal_delay_length_minus1 + 1;

    for(uint8_t i = 0; i <= pSPS->vui_param.hrd_param.cpb_cnt_minus1[0]; ++i)
    {
      pBufPeriod->initial_cpb_removal_delay[i] = u(pRP, syntax_size);
      pBufPeriod->initial_cpb_removal_delay_offset[i] = u(pRP, syntax_size);
    }
  }

  if(pSPS->vui_param.hrd_param.vcl_hrd_parameters_present_flag)
  {
    uint8_t syntax_size = pSPS->vui_param.hrd_param.initial_cpb_removal_delay_length_minus1 + 1;

    for(uint8_t i = 0; i <= pSPS->vui_param.hrd_param.cpb_cnt_minus1[0]; ++i)
    {
      pBufPeriod->initial_cpb_removal_delay[i] = u(pRP, syntax_size);
      pBufPeriod->initial_cpb_removal_delay_offset[i] = u(pRP, syntax_size);
    }
  }

  return true;
}

/*****************************************************************************/
static bool ParseSeiPayload(SeiParserParam* p, AL_TRbspParser* pRP, AL_ESeiPayloadType ePayloadType, int iPayloadSize, bool* bCanSendToUser, bool* bParsed)
{
  (void)iPayloadSize;
  bool bParsingOk = true;
  *bCanSendToUser = true;
  *bParsed = true;
  AL_TAvcAup* aup = &p->pIAup->avcAup;
  switch(ePayloadType)
  {
  case SEI_PTYPE_BUFFERING_PERIOD:
  {
    AL_TAvcBufPeriod tBufferingPeriod;
    bParsingOk = SeiBufferingPeriod(pRP, aup->pSPS, &tBufferingPeriod, &aup->pActiveSPS);
    break;
  }
  default:
    *bCanSendToUser = false;
    *bParsed = false;
    break;
  }

  return bParsingOk;
}

/*****************************************************************************/
bool AL_AVC_ParseSEI(AL_TAup* pIAup, AL_TRbspParser* pRP, bool bIsPrefix, AL_CB_ParsedSei* cb, AL_TSeiMetaData* pMeta)
{
  skipAllZerosAndTheNextByte(pRP);

  u(pRP, 8); // Skip NUT

  SeiParserParam tSeiParserParam = { pIAup, bIsPrefix, cb, pMeta };
  SeiParserCB tSeiParserCb = { ParseSeiPayload, &tSeiParserParam };

  do
  {
    if(!ParseSeiHeader(pRP, &tSeiParserCb))
      return false;
  }
  while(more_rbsp_data(pRP));

  rbsp_trailing_bits(pRP);

  return true;
}

/*****************************************************************************/
void AL_AVC_GetCropInfo(AL_TAvcSps const* pSPS, AL_TCropInfo* pCropInfo)
{
  if(pSPS->frame_cropping_flag)
  {
    pCropInfo->bCropping = true;

    int iCropUnitX = 0;
    int iCropUnitY = 0;
    switch(pSPS->chroma_format_idc)
    {
    case 0:  // monochrome
      DBG_ASSERT_COND((pSPS->separate_colour_plane_flag == 0) && " pSPS->separate_colour_plane_flag != 0 is not allowed in Monochrome");
      iCropUnitX = 1;
      iCropUnitY = 1;
      break;

    case 1:  // 4:2:0
      DBG_ASSERT_COND((pSPS->separate_colour_plane_flag == 0) && " pSPS->separate_colour_plane_flag != 0 is not allowed in 4:2:0");
      iCropUnitX = 2;
      iCropUnitY = 2;
      break;

    case 2:  // 4:2:2
      DBG_ASSERT_COND((pSPS->separate_colour_plane_flag == 0) && " pSPS->separate_colour_plane_flag != 0 is not allowed in 4:2:2");
      iCropUnitX = 2;
      iCropUnitY = 1;
      break;

    case 3:  // 4:4:4
      iCropUnitX = 1;
      iCropUnitY = 1;
      break;

    default:
      DBG_ASSERT_COND(0 && "invalid pSPS->chroma_format_idc");
    }

    iCropUnitY *= (2 - pSPS->frame_mbs_only_flag);

    pCropInfo->uCropOffsetLeft = iCropUnitX * pSPS->frame_crop_left_offset;
    pCropInfo->uCropOffsetRight = iCropUnitX * pSPS->frame_crop_right_offset;

    pCropInfo->uCropOffsetTop = iCropUnitY * pSPS->frame_crop_top_offset;
    pCropInfo->uCropOffsetBottom = iCropUnitY * pSPS->frame_crop_bottom_offset;
  }
  else
  {
    ResetCropInfo(pCropInfo);
  }
}
