// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "HevcParser.h"
#include "lib_parsing/Concealment.h"
#include "lib_rtos/lib_rtos.h"
#include "lib_common/Utils.h"
#include "lib_common/SeiInternal.h"
#include "lib_common_dec/RbspParser.h"
#include "SeiParser.h"

#define CONCEAL_LEVEL_IDC 60 * 3

/*****************************************************************************/
static void initPps(AL_THevcPps* pPPS)
{
  Rtos_Memset(pPPS->scaling_list_param.UseDefaultScalingMatrixFlag, 0, sizeof(pPPS->scaling_list_param.UseDefaultScalingMatrixFlag));
  Rtos_Memset(pPPS->cb_qp_offset_list, 0, sizeof(pPPS->cb_qp_offset_list));
  Rtos_Memset(pPPS->cr_qp_offset_list, 0, sizeof(pPPS->cr_qp_offset_list));

  pPPS->loop_filter_across_tiles_enabled_flag = 1;
  pPPS->lists_modification_present_flag = 1;
  pPPS->num_tile_columns_minus1 = 0;
  pPPS->num_tile_rows_minus1 = 0;
  pPPS->deblocking_filter_override_enabled_flag = 0;
  pPPS->pps_deblocking_filter_disabled_flag = 0;
  pPPS->pps_beta_offset_div2 = 0;
  pPPS->pps_tc_offset_div2 = 0;

  pPPS->pps_extension_7bits = 0;
  pPPS->pps_range_extension_flag = 0;
  pPPS->log2_transform_skip_block_size_minus2 = 0;
  pPPS->cross_component_prediction_enabled_flag = 0;
  pPPS->diff_cu_chroma_qp_offset_depth = 0;
  pPPS->chroma_qp_offset_list_enabled_flag = 0;
  pPPS->log2_sao_offset_scale_luma = 0;
  pPPS->log2_sao_offset_scale_chroma = 0;

  pPPS->bConceal = true;
}

/*****************************************************************************/
AL_PARSE_RESULT AL_HEVC_ParsePPS(AL_TAup* pIAup, AL_TRbspParser* pRP, uint16_t* pPpsId)
{
  skipAllZerosAndTheNextByte(pRP);
  u(pRP, 16); // Skip NUT + temporal_id

  uint16_t pps_id = ue(pRP);

  if(pPpsId)
    *pPpsId = pps_id;

  COMPLY(pps_id < AL_HEVC_MAX_PPS);

  AL_THevcAup* aup = &pIAup->hevcAup;
  AL_THevcPps* pPPS = &aup->pPPS[pps_id];

  // default values
  initPps(pPPS);

  pPPS->pps_pic_parameter_set_id = pps_id;
  pPPS->pps_seq_parameter_set_id = ue(pRP);

  COMPLY(pPPS->pps_seq_parameter_set_id < AL_HEVC_MAX_SPS);

  pPPS->pSPS = &aup->pSPS[pPPS->pps_seq_parameter_set_id];

  COMPLY(!pPPS->pSPS->bConceal);

  uint16_t uLCUPicWidth = pPPS->pSPS->PicWidthInCtbs;
  uint16_t uLCUPicHeight = pPPS->pSPS->PicHeightInCtbs;

  pPPS->dependent_slice_segments_enabled_flag = u(pRP, 1);
  pPPS->output_flag_present_flag = u(pRP, 1);
  pPPS->num_extra_slice_header_bits = u(pRP, 3);
  pPPS->sign_data_hiding_flag = u(pRP, 1);
  pPPS->cabac_init_present_flag = u(pRP, 1);

  pPPS->num_ref_idx_l0_default_active_minus1 = Clip3(ue(pRP), 0, AL_HEVC_MAX_REF_IDX);
  pPPS->num_ref_idx_l1_default_active_minus1 = Clip3(ue(pRP), 0, AL_HEVC_MAX_REF_IDX);

  uint16_t QpBdOffset = 6 * pPPS->pSPS->bit_depth_luma_minus8;
  pPPS->init_qp_minus26 = Clip3(se(pRP), -(26 + QpBdOffset), AL_MAX_INIT_QP);

  pPPS->constrained_intra_pred_flag = u(pRP, 1);
  pPPS->transform_skip_enabled_flag = u(pRP, 1);

  pPPS->cu_qp_delta_enabled_flag = u(pRP, 1);

  if(pPPS->cu_qp_delta_enabled_flag)
    pPPS->diff_cu_qp_delta_depth = Clip3(ue(pRP), 0, pPPS->pSPS->log2_diff_max_min_luma_coding_block_size);

  pPPS->pps_cb_qp_offset = Clip3(se(pRP), AL_MIN_QP_OFFSET, AL_MAX_QP_OFFSET);
  pPPS->pps_cr_qp_offset = Clip3(se(pRP), AL_MIN_QP_OFFSET, AL_MAX_QP_OFFSET);
  pPPS->pps_slice_chroma_qp_offsets_present_flag = u(pRP, 1);

  pPPS->weighted_pred_flag = u(pRP, 1);
  pPPS->weighted_bipred_flag = u(pRP, 1);

  pPPS->transquant_bypass_enabled_flag = u(pRP, 1);
  pPPS->tiles_enabled_flag = u(pRP, 1);
  pPPS->entropy_coding_sync_enabled_flag = u(pRP, 1);

  if(pPPS->tiles_enabled_flag)
  {
    pPPS->num_tile_columns_minus1 = ue(pRP);
    pPPS->num_tile_rows_minus1 = ue(pRP);
    pPPS->uniform_spacing_flag = u(pRP, 1);

    COMPLY(!(pPPS->num_tile_columns_minus1 >= uLCUPicWidth || pPPS->num_tile_rows_minus1 >= uLCUPicHeight ||
             pPPS->num_tile_columns_minus1 >= AL_MAX_COLUMNS_TILE || pPPS->num_tile_rows_minus1 >= AL_MAX_ROWS_TILE));

    if(!pPPS->uniform_spacing_flag)
    {
      uint16_t uClmnOffset = 0;
      uint16_t uLineOffset = 0;

      for(uint8_t i = 0; i < pPPS->num_tile_columns_minus1; ++i)
      {
        pPPS->tile_column_width[i] = ue(pRP) + 1;
        uClmnOffset += pPPS->tile_column_width[i];
      }

      COMPLY(uClmnOffset < uLCUPicWidth);

      for(uint8_t i = 0; i < pPPS->num_tile_rows_minus1; ++i)
      {
        pPPS->tile_row_height[i] = ue(pRP) + 1;
        uLineOffset += pPPS->tile_row_height[i];
      }

      COMPLY(uLineOffset < uLCUPicHeight);

      pPPS->tile_column_width[pPPS->num_tile_columns_minus1] = uLCUPicWidth - uClmnOffset;
      pPPS->tile_row_height[pPPS->num_tile_rows_minus1] = uLCUPicHeight - uLineOffset;
    }
    else /* tile of same size */
    {
      uint16_t num_clmn = pPPS->num_tile_columns_minus1 + 1;
      uint16_t num_line = pPPS->num_tile_rows_minus1 + 1;

      for(uint8_t i = 0; i <= pPPS->num_tile_columns_minus1; ++i)
        pPPS->tile_column_width[i] = (((i + 1) * uLCUPicWidth) / num_clmn) - ((i * uLCUPicWidth) / num_clmn);

      for(uint8_t i = 0; i <= pPPS->num_tile_rows_minus1; ++i)
        pPPS->tile_row_height[i] = (((i + 1) * uLCUPicHeight) / num_line) - ((i * uLCUPicHeight) / num_line);
    }

    /* register tile topology within the frame */
    for(uint8_t i = 0; i <= pPPS->num_tile_rows_minus1; ++i)
    {
      for(uint8_t j = 0; j <= pPPS->num_tile_columns_minus1; ++j)
      {
        uint8_t line = 0;
        uint8_t clmn = 0;
        uint8_t uClmn = 0;
        uint8_t uLine = 0;

        while(line < i)
          uLine += pPPS->tile_row_height[line++];

        while(clmn < j)
          uClmn += pPPS->tile_column_width[clmn++];

        pPPS->TileTopology[(i * (pPPS->num_tile_columns_minus1 + 1)) + j] = uLine * uLCUPicWidth + uClmn;
      }
    }

    pPPS->loop_filter_across_tiles_enabled_flag = u(pRP, 1);
  }

  pPPS->loop_filter_across_slices_enabled_flag = u(pRP, 1);
  pPPS->deblocking_filter_control_present_flag = u(pRP, 1);

  if(pPPS->deblocking_filter_control_present_flag)
  {
    pPPS->deblocking_filter_override_enabled_flag = u(pRP, 1);
    pPPS->pps_deblocking_filter_disabled_flag = u(pRP, 1);

    if(!pPPS->pps_deblocking_filter_disabled_flag)
    {
      pPPS->pps_beta_offset_div2 = Clip3(se(pRP), AL_MIN_DBF_PARAM, AL_MAX_DBF_PARAM);
      pPPS->pps_tc_offset_div2 = Clip3(se(pRP), AL_MIN_DBF_PARAM, AL_MAX_DBF_PARAM);
    }
  }

  pPPS->pps_scaling_list_data_present_flag = u(pRP, 1);

  if(pPPS->pps_scaling_list_data_present_flag)
    hevc_scaling_list_data(&pPPS->scaling_list_param, pRP);
  else // get scaling_list data from associated sps
    pPPS->scaling_list_param = pPPS->pSPS->scaling_list_param;

  pPPS->lists_modification_present_flag = u(pRP, 1);
  pPPS->log2_parallel_merge_level_minus2 = Clip3(ue(pRP), 0, pPPS->pSPS->Log2CtbSize - 2);

  pPPS->slice_segment_header_extension_present_flag = u(pRP, 1);
  pPPS->pps_extension_present_flag = u(pRP, 1);

  if(pPPS->pps_extension_present_flag)
  {
    pPPS->pps_range_extension_flag = u(pRP, 1);
    pPPS->pps_extension_7bits = u(pRP, 7);
  }

  if(pPPS->pps_range_extension_flag)
  {
    if(pPPS->transform_skip_enabled_flag)
      pPPS->log2_transform_skip_block_size_minus2 = ue(pRP);
    pPPS->cross_component_prediction_enabled_flag = u(pRP, 1);
    pPPS->chroma_qp_offset_list_enabled_flag = u(pRP, 1);

    if(pPPS->chroma_qp_offset_list_enabled_flag)
    {
      pPPS->diff_cu_chroma_qp_offset_depth = ue(pRP);
      pPPS->chroma_qp_offset_list_len_minus1 = Clip3(ue(pRP), 0, 5);

      for(int i = 0; i <= pPPS->chroma_qp_offset_list_len_minus1; ++i)
      {
        pPPS->cb_qp_offset_list[i] = se(pRP);
        pPPS->cr_qp_offset_list[i] = se(pRP);
      }
    }
    pPPS->log2_sao_offset_scale_luma = ue(pRP);
    pPPS->log2_sao_offset_scale_chroma = ue(pRP);
  }

  if(pPPS->pps_extension_7bits) // pps_extension_flag
  {
    while(more_rbsp_data(pRP))
      skip(pRP, 1); // pps_extension_data_flag
  }

  COMPLY(rbsp_trailing_bits(pRP));

  return AL_OK;
}

/*****************************************************************************/
static bool AL_HEVC_sComputeRefPicSetVariables(AL_THevcSps* pSPS, uint8_t RpsIdx)
{
  uint8_t num_negative = 0, num_positive = 0;
  AL_TRefPicSet ref_pic_set = pSPS->short_term_ref_pic_set[RpsIdx];

  if(ref_pic_set.inter_ref_pic_set_prediction_flag)
  {
    uint8_t RIdx = RpsIdx - (ref_pic_set.delta_idx_minus1 + 1);
    int32_t DeltaRPS = (1 - (ref_pic_set.delta_rps_sign << 1)) * (ref_pic_set.abs_delta_rps_minus1 + 1);

    if(RIdx > MAX_REF_PIC_SET)
      return false;

    // num negative pics computation
    for(int j = pSPS->NumPositivePics[RIdx] - 1; j >= 0; --j)
    {
      int32_t delta_poc = pSPS->DeltaPocS1[RIdx][j] + DeltaRPS;

      if(delta_poc < 0 && ref_pic_set.use_delta_flag[pSPS->NumNegativePics[RIdx] + j])
      {
        if(num_negative >= MAX_REF)
          return false;

        pSPS->DeltaPocS0[RpsIdx][num_negative] = delta_poc;
        pSPS->UsedByCurrPicS0[RpsIdx][num_negative++] = ref_pic_set.used_by_curr_pic_flag[pSPS->NumNegativePics[RIdx] + j];
      }
    }

    if(DeltaRPS < 0 && ref_pic_set.use_delta_flag[pSPS->NumDeltaPocs[RIdx]])
    {
      if(num_negative >= MAX_REF)
        return false;

      pSPS->DeltaPocS0[RpsIdx][num_negative] = DeltaRPS;
      pSPS->UsedByCurrPicS0[RpsIdx][num_negative++] = ref_pic_set.used_by_curr_pic_flag[pSPS->NumDeltaPocs[RIdx]];
    }

    for(int j = 0; j < pSPS->NumNegativePics[RIdx]; ++j)
    {
      int32_t delta_poc = pSPS->DeltaPocS0[RIdx][j] + DeltaRPS;

      if(delta_poc < 0 && ref_pic_set.use_delta_flag[j])
      {
        if(num_negative >= MAX_REF)
          return false;

        pSPS->DeltaPocS0[RpsIdx][num_negative] = delta_poc;
        pSPS->UsedByCurrPicS0[RpsIdx][num_negative++] = ref_pic_set.used_by_curr_pic_flag[j];
      }
    }

    pSPS->NumNegativePics[RpsIdx] = num_negative;

    // num positive pics computation
    for(int j = pSPS->NumNegativePics[RIdx] - 1; j >= 0; --j)
    {
      int32_t delta_poc = pSPS->DeltaPocS0[RIdx][j] + DeltaRPS;

      if(delta_poc > 0 && ref_pic_set.use_delta_flag[j])
      {
        if(num_negative >= MAX_REF)
          return false;

        pSPS->DeltaPocS1[RpsIdx][num_positive] = delta_poc;
        pSPS->UsedByCurrPicS1[RpsIdx][num_positive++] = ref_pic_set.used_by_curr_pic_flag[j];
      }
    }

    if(DeltaRPS > 0 && ref_pic_set.use_delta_flag[pSPS->NumDeltaPocs[RIdx]])
    {
      if(num_negative >= MAX_REF)
        return false;

      pSPS->DeltaPocS1[RpsIdx][num_positive] = DeltaRPS;
      pSPS->UsedByCurrPicS1[RpsIdx][num_positive++] = ref_pic_set.used_by_curr_pic_flag[pSPS->NumDeltaPocs[RIdx]];
    }

    for(int j = 0; j < pSPS->NumPositivePics[RIdx]; ++j)
    {
      int32_t delta_poc = pSPS->DeltaPocS1[RIdx][j] + DeltaRPS;

      if(delta_poc > 0 && ref_pic_set.use_delta_flag[pSPS->NumNegativePics[RIdx] + j])
      {
        if(num_negative >= MAX_REF)
          return false;

        pSPS->DeltaPocS1[RpsIdx][num_positive] = delta_poc;
        pSPS->UsedByCurrPicS1[RpsIdx][num_positive++] = ref_pic_set.used_by_curr_pic_flag[pSPS->NumNegativePics[RIdx] + j];
      }
    }

    pSPS->NumPositivePics[RpsIdx] = num_positive;
  }
  else
  {
    pSPS->NumNegativePics[RpsIdx] = ref_pic_set.num_negative_pics;
    pSPS->NumPositivePics[RpsIdx] = ref_pic_set.num_positive_pics;

    pSPS->UsedByCurrPicS0[RpsIdx][0] = ref_pic_set.used_by_curr_pic_s0_flag[0];
    pSPS->UsedByCurrPicS1[RpsIdx][0] = ref_pic_set.used_by_curr_pic_s1_flag[0];

    pSPS->DeltaPocS0[RpsIdx][0] = -(ref_pic_set.delta_poc_s0_minus1[0] + 1);
    pSPS->DeltaPocS1[RpsIdx][0] = ref_pic_set.delta_poc_s1_minus1[0] + 1;

    for(int j = 1; j < ref_pic_set.num_negative_pics; ++j)
    {
      pSPS->UsedByCurrPicS0[RpsIdx][j] = ref_pic_set.used_by_curr_pic_s0_flag[j];
      pSPS->DeltaPocS0[RpsIdx][j] = pSPS->DeltaPocS0[RpsIdx][j - 1] - (ref_pic_set.delta_poc_s0_minus1[j] + 1);
    }

    for(int j = 1; j < ref_pic_set.num_positive_pics; ++j)
    {
      pSPS->UsedByCurrPicS1[RpsIdx][j] = ref_pic_set.used_by_curr_pic_s1_flag[j];
      pSPS->DeltaPocS1[RpsIdx][j] = pSPS->DeltaPocS1[RpsIdx][j - 1] + (ref_pic_set.delta_poc_s1_minus1[j] + 1);
    }
  }
  pSPS->NumDeltaPocs[RpsIdx] = pSPS->NumNegativePics[RpsIdx] + pSPS->NumPositivePics[RpsIdx];

  return true;
}

/*****************************************************************************/
static void initSps(AL_THevcSps* pSPS)
{
  Rtos_Memset(pSPS, 0, sizeof(AL_THevcSps));

  pSPS->chroma_format_idc = 1;

  pSPS->bConceal = true;
}

/*****************************************************************************/
static void initVui(AL_TVuiParam* pVuiParam, AL_THevcProfilevel* pProfileAndLevel)
{
  // The full SPS is already memset to 0 in InitSPS. Only nonzero values are overwritten here
  pVuiParam->video_format = 5;
  pVuiParam->colour_primaries = 2;
  pVuiParam->transfer_characteristics = 2;
  pVuiParam->frame_field_info_present_flag = (pProfileAndLevel->general_progressive_source_flag && pProfileAndLevel->general_interlaced_source_flag) ? 1 : 0;

  pVuiParam->max_bytes_per_pic_denom = 2;
  pVuiParam->max_bits_per_min_cu_denom = 1;
  pVuiParam->log2_max_mv_length_horizontal = 15;
  pVuiParam->log2_max_mv_length_vertical = 15;

  pVuiParam->hrd_param.du_cpb_removal_delay_increment_length_minus1 = 23;
  pVuiParam->hrd_param.initial_cpb_removal_delay_length_minus1 = 23;
  pVuiParam->hrd_param.dpb_output_delay_length_minus1 = 23;
}

/*****************************************************************************/
AL_PARSE_RESULT AL_HEVC_ParseSPS(AL_TRbspParser* pRP, AL_THevcSps* pSPS)
{
  skipAllZerosAndTheNextByte(pRP);
  u(pRP, 16); // Skip NUT + temporal_id

  initSps(pSPS);

  pSPS->sps_video_parameter_set_id = u(pRP, 4);
  COMPLY_ID(pSPS->sps_video_parameter_set_id < AL_HEVC_MAX_VPS);

  int max_sub_layers = Clip3(u(pRP, 3), 0, MAX_SUB_LAYER - 1);
  pSPS->sps_max_sub_layers_minus1 = max_sub_layers;
  pSPS->sps_temporal_id_nesting_flag = u(pRP, 1);

  hevc_profile_tier_level(&pSPS->profile_and_level, pSPS->sps_max_sub_layers_minus1, pRP);

  if(pSPS->profile_and_level.general_level_idc == 0)
    pSPS->profile_and_level.general_level_idc = CONCEAL_LEVEL_IDC;

  pSPS->sps_seq_parameter_set_id = ue(pRP);
  COMPLY_ID(pSPS->sps_seq_parameter_set_id < AL_HEVC_MAX_SPS);

  // default VUI values
  initVui(&pSPS->vui_param, &pSPS->profile_and_level);

  pSPS->chroma_format_idc = ue(pRP);

  COMPLY(pSPS->chroma_format_idc < AL_CHROMA_MAX_ENUM);

  if(pSPS->chroma_format_idc == 3)
    pSPS->separate_colour_plane_flag = u(pRP, 1);

  if(pSPS->separate_colour_plane_flag)
    pSPS->ChromaArrayType = 0;
  else
    pSPS->ChromaArrayType = pSPS->chroma_format_idc;

  pSPS->pic_width_in_luma_samples = ue(pRP);
  pSPS->pic_height_in_luma_samples = ue(pRP);

  pSPS->conformance_window_flag = u(pRP, 1);

  if(pSPS->conformance_window_flag)
  {
    pSPS->conf_win_left_offset = ue(pRP);
    pSPS->conf_win_right_offset = ue(pRP);
    pSPS->conf_win_top_offset = ue(pRP);
    pSPS->conf_win_bottom_offset = ue(pRP);

    if(pSPS->conf_win_bottom_offset + pSPS->conf_win_top_offset >= pSPS->pic_height_in_luma_samples)
    {
      pSPS->conf_win_top_offset = 0;
      pSPS->conf_win_bottom_offset = 0;
    }

    if(pSPS->conf_win_left_offset + pSPS->conf_win_right_offset >= pSPS->pic_width_in_luma_samples)
    {
      pSPS->conf_win_left_offset = 0;
      pSPS->conf_win_right_offset = 0;
    }
  }

  pSPS->bit_depth_luma_minus8 = Clip3(ue(pRP), 0, MAX_BIT_DEPTH);
  pSPS->bit_depth_chroma_minus8 = Clip3(ue(pRP), 0, MAX_BIT_DEPTH);

  pSPS->log2_max_slice_pic_order_cnt_lsb_minus4 = ue(pRP);

  COMPLY(pSPS->log2_max_slice_pic_order_cnt_lsb_minus4 <= MAX_POC_LSB);

  pSPS->sps_sub_layer_ordering_info_present_flag = u(pRP, 1);
  int layer_offset = pSPS->sps_sub_layer_ordering_info_present_flag ? 0 : max_sub_layers;

  for(int i = layer_offset; i <= max_sub_layers; ++i)
  {
    pSPS->sps_max_dec_pic_buffering_minus1[i] = ue(pRP);
    pSPS->sps_max_num_reorder_pics[i] = ue(pRP);
    pSPS->sps_max_latency_increase_plus1[i] = ue(pRP);
  }

  pSPS->log2_min_luma_coding_block_size_minus3 = ue(pRP);
  pSPS->Log2MinCbSize = pSPS->log2_min_luma_coding_block_size_minus3 + 3;

  COMPLY(pSPS->Log2MinCbSize <= 6);

  pSPS->log2_diff_max_min_luma_coding_block_size = ue(pRP);
  pSPS->Log2CtbSize = pSPS->Log2MinCbSize + pSPS->log2_diff_max_min_luma_coding_block_size;

  COMPLY(pSPS->Log2CtbSize <= 6);
  COMPLY(pSPS->Log2CtbSize >= 4);

  pSPS->log2_min_transform_block_size_minus2 = ue(pRP);
  int Log2MinTransfoSize = pSPS->log2_min_transform_block_size_minus2 + 2;
  pSPS->log2_diff_max_min_transform_block_size = ue(pRP);

  COMPLY(pSPS->log2_min_transform_block_size_minus2 <= pSPS->log2_min_luma_coding_block_size_minus3);
  COMPLY(pSPS->log2_diff_max_min_transform_block_size <= Min(pSPS->Log2CtbSize, 5) - Log2MinTransfoSize);

  pSPS->max_transform_hierarchy_depth_inter = ue(pRP);
  pSPS->max_transform_hierarchy_depth_intra = ue(pRP);

  COMPLY(pSPS->max_transform_hierarchy_depth_inter <= (pSPS->Log2CtbSize - Log2MinTransfoSize));
  COMPLY(pSPS->max_transform_hierarchy_depth_intra <= (pSPS->Log2CtbSize - Log2MinTransfoSize));

  pSPS->scaling_list_enabled_flag = u(pRP, 1);

  // check if NAL isn't empty
  COMPLY(more_rbsp_data(pRP));

  if(pSPS->scaling_list_enabled_flag)
  {
    pSPS->sps_scaling_list_data_present_flag = u(pRP, 1);

    if(pSPS->sps_scaling_list_data_present_flag)
      hevc_scaling_list_data(&pSPS->scaling_list_param, pRP);
    else
      for(int i = 0; i < 20; ++i)
        pSPS->scaling_list_param.UseDefaultScalingMatrixFlag[i] = 1;
  }

  pSPS->amp_enabled_flag = u(pRP, 1);
  pSPS->sample_adaptive_offset_enabled_flag = u(pRP, 1);

  pSPS->pcm_enabled_flag = u(pRP, 1);

  if(pSPS->pcm_enabled_flag)
  {
    pSPS->pcm_sample_bit_depth_luma_minus1 = u(pRP, 4);
    pSPS->pcm_sample_bit_depth_chroma_minus1 = u(pRP, 4);

    COMPLY(pSPS->pcm_sample_bit_depth_luma_minus1 <= pSPS->bit_depth_luma_minus8 + 7);
    COMPLY(pSPS->pcm_sample_bit_depth_chroma_minus1 <= pSPS->bit_depth_chroma_minus8 + 7);

    pSPS->log2_min_pcm_luma_coding_block_size_minus3 = Clip3(ue(pRP), Min(pSPS->log2_min_luma_coding_block_size_minus3, 2), Min(pSPS->Log2CtbSize - 3, 2));

    COMPLY(pSPS->log2_min_pcm_luma_coding_block_size_minus3 >= Min(pSPS->log2_min_luma_coding_block_size_minus3, 2));
    COMPLY(pSPS->log2_min_pcm_luma_coding_block_size_minus3 <= Min(pSPS->Log2CtbSize - 3, 2));

    pSPS->log2_diff_max_min_pcm_luma_coding_block_size = Clip3(ue(pRP), 0, Min(pSPS->Log2CtbSize - 3, 2) - pSPS->log2_min_pcm_luma_coding_block_size_minus3);

    COMPLY(pSPS->log2_diff_max_min_pcm_luma_coding_block_size <= (Min(pSPS->Log2CtbSize - 3, 2) - pSPS->log2_min_pcm_luma_coding_block_size_minus3));

    pSPS->pcm_loop_filter_disabled_flag = u(pRP, 1);
  }

  pSPS->num_short_term_ref_pic_sets = ue(pRP);

  COMPLY(pSPS->num_short_term_ref_pic_sets <= MAX_REF_PIC_SET);

  for(int i = 0; i < pSPS->num_short_term_ref_pic_sets; ++i)
  {
    // check if NAL isn't empty
    COMPLY(more_rbsp_data(pRP));
    COMPLY(AL_HEVC_short_term_ref_pic_set(pSPS, i, pRP));

    pSPS->sps_max_dec_pic_buffering_minus1[pSPS->sps_max_sub_layers_minus1] =
      Max(pSPS->sps_max_dec_pic_buffering_minus1[pSPS->sps_max_sub_layers_minus1], pSPS->NumDeltaPocs[i]);
  }

  pSPS->long_term_ref_pics_present_flag = u(pRP, 1);

  if(pSPS->long_term_ref_pics_present_flag)
  {
    uint8_t syntax_size = pSPS->log2_max_slice_pic_order_cnt_lsb_minus4 + 4;
    pSPS->num_long_term_ref_pics_sps = ue(pRP);

    COMPLY(pSPS->num_long_term_ref_pics_sps <= MAX_LONG_TERM_PIC);

    for(int i = 0; i < pSPS->num_long_term_ref_pics_sps; ++i)
    {
      pSPS->lt_ref_pic_poc_lsb_sps[i] = u(pRP, syntax_size);
      pSPS->used_by_curr_pic_lt_sps_flag[i] = u(pRP, 1);
    }
  }
  pSPS->sps_temporal_mvp_enabled_flag = u(pRP, 1);
  pSPS->strong_intra_smoothing_enabled_flag = u(pRP, 1);

  pSPS->vui_parameters_present_flag = u(pRP, 1);

  // check if NAL isn't empty
  COMPLY(more_rbsp_data(pRP));

  if(pSPS->vui_parameters_present_flag)
    hevc_vui_parameters(&pSPS->vui_param, pSPS->sps_max_sub_layers_minus1, pRP);

  pSPS->sps_extension_present_flag = u(pRP, 1);

  if(pSPS->sps_extension_present_flag)
  {
    pSPS->sps_range_extension_flag = u(pRP, 1);
    pSPS->sps_extension_7bits = u(pRP, 7);
  }

  if(pSPS->sps_range_extension_flag)
  {
    pSPS->transform_skip_rotation_enabled_flag = u(pRP, 1);
    pSPS->transform_skip_context_enabled_flag = u(pRP, 1);
    pSPS->implicit_rdpcm_enabled_flag = u(pRP, 1);
    pSPS->explicit_rdpcm_enabled_flag = u(pRP, 1);
    pSPS->extended_precision_processing_flag = u(pRP, 1);
    pSPS->intra_smoothing_disabled_flag = u(pRP, 1);
    pSPS->high_precision_offsets_enabled_flag = u(pRP, 1);
    pSPS->persistent_rice_adaptation_enabled_flag = u(pRP, 1);
    pSPS->cabac_bypass_alignment_enabled_flag = u(pRP, 1);
  }

  if(pSPS->sps_extension_7bits) // sps_extension_flag
  {
    while(more_rbsp_data(pRP))
      skip(pRP, 1); // sps_extension_data_flag
  }

  // Compute variables
  pSPS->PicWidthInCtbs = (pSPS->pic_width_in_luma_samples + ((1 << pSPS->Log2CtbSize) - 1)) >> pSPS->Log2CtbSize;
  pSPS->PicHeightInCtbs = (pSPS->pic_height_in_luma_samples + ((1 << pSPS->Log2CtbSize) - 1)) >> pSPS->Log2CtbSize;

  COMPLY_WITH_LOG(pSPS->PicWidthInCtbs >= 2, "SPS width less than 2 CTBs is not supported\n");
  COMPLY_WITH_LOG(pSPS->PicHeightInCtbs >= 2, "SPS height less than 2 CTBs is not supported\n");

  pSPS->PicWidthInMinCbs = pSPS->pic_width_in_luma_samples >> pSPS->Log2MinCbSize;
  pSPS->PicHeightInMinCbs = pSPS->pic_height_in_luma_samples >> pSPS->Log2MinCbSize;

  pSPS->SpsMaxLatency = pSPS->sps_max_latency_increase_plus1[max_sub_layers] ? pSPS->sps_max_num_reorder_pics[max_sub_layers] + pSPS->sps_max_latency_increase_plus1[max_sub_layers] - 1 : UINT32_MAX;

  pSPS->MaxPicOrderCntLsb = 1 << (pSPS->log2_max_slice_pic_order_cnt_lsb_minus4 + 4);

  pSPS->WpOffsetBdShiftY = pSPS->high_precision_offsets_enabled_flag ? 0 : pSPS->bit_depth_luma_minus8;
  pSPS->WpOffsetBdShiftC = pSPS->high_precision_offsets_enabled_flag ? 0 : pSPS->bit_depth_chroma_minus8;
  pSPS->WpOffsetHalfRangeY = 1 << (pSPS->high_precision_offsets_enabled_flag ? pSPS->bit_depth_luma_minus8 + 7 : 7);
  pSPS->WpOffsetHalfRangeC = 1 << (pSPS->high_precision_offsets_enabled_flag ? pSPS->bit_depth_chroma_minus8 + 7 : 7);

  COMPLY(rbsp_trailing_bits(pRP));

  pSPS->bConceal = false;

  return AL_OK;
}

/*****************************************************************************/
bool AL_HEVC_short_term_ref_pic_set(AL_THevcSps* pSPS, uint8_t RpsIdx, AL_TRbspParser* pRP)
{
  uint8_t RIdx;
  AL_TRefPicSet* pRefPicSet = &pSPS->short_term_ref_pic_set[RpsIdx];

  // default values
  pRefPicSet->delta_idx_minus1 = 0;
  pRefPicSet->inter_ref_pic_set_prediction_flag = 0;

  if(RpsIdx)
    pRefPicSet->inter_ref_pic_set_prediction_flag = u(pRP, 1);

  if(pRefPicSet->inter_ref_pic_set_prediction_flag)
  {
    if(RpsIdx == pSPS->num_short_term_ref_pic_sets)
      pRefPicSet->delta_idx_minus1 = ue(pRP);
    pRefPicSet->delta_rps_sign = u(pRP, 1);
    pRefPicSet->abs_delta_rps_minus1 = ue(pRP);

    RIdx = RpsIdx - (pRefPicSet->delta_idx_minus1 + 1);

    if(RIdx > MAX_REF_PIC_SET)
      return false;

    for(uint8_t j = 0; j <= pSPS->NumDeltaPocs[RIdx]; ++j)
    {
      pRefPicSet->use_delta_flag[j] = 1;
      pRefPicSet->used_by_curr_pic_flag[j] = u(pRP, 1);

      if(!pRefPicSet->used_by_curr_pic_flag[j])
        pRefPicSet->use_delta_flag[j] = u(pRP, 1);
    }
  }
  else
  {
    pRefPicSet->num_negative_pics = ue(pRP);

    if(pRefPicSet->num_negative_pics > MAX_REF)
      return false;

    pRefPicSet->num_positive_pics = ue(pRP);

    if(pRefPicSet->num_negative_pics > MAX_REF)
      return false;

    for(uint8_t j = 0; j < pRefPicSet->num_negative_pics; ++j)
    {
      pRefPicSet->delta_poc_s0_minus1[j] = ue(pRP);
      pRefPicSet->used_by_curr_pic_s0_flag[j] = u(pRP, 1);
    }

    for(uint8_t j = 0; j < pRefPicSet->num_positive_pics; ++j)
    {
      pRefPicSet->delta_poc_s1_minus1[j] = ue(pRP);
      pRefPicSet->used_by_curr_pic_s1_flag[j] = u(pRP, 1);
    }
  }
  return AL_HEVC_sComputeRefPicSetVariables(pSPS, RpsIdx);
}

/*****************************************************************************/
AL_PARSE_RESULT AL_HEVC_ParseVPS(AL_TAup* pIAup, AL_TRbspParser* pRP)
{
  AL_THevcVps* pVPS;

  skipAllZerosAndTheNextByte(pRP);

  u(pRP, 16); // Skip NUT + temporal_id

  int vps_id = u(pRP, 4);

  if(vps_id >= AL_HEVC_MAX_VPS)
    return AL_UNSUPPORTED;

  pVPS = &pIAup->hevcAup.pVPS[vps_id];
  pVPS->vps_video_parameter_set_id = vps_id;

  pVPS->vps_base_layer_internal_flag = u(pRP, 1);
  pVPS->vps_base_layer_available_flag = u(pRP, 1);
  pVPS->vps_max_layers_minus1 = u(pRP, 6);

  pVPS->vps_max_sub_layers_minus1 = u(pRP, 3);
  pVPS->vps_temporal_id_nesting_flag = u(pRP, 1);
  skip(pRP, 16); // vps_reserved_0xffff_16bits

  hevc_profile_tier_level(&pVPS->profile_and_level[0], pVPS->vps_max_sub_layers_minus1, pRP);

  if(pVPS->profile_and_level[0].general_level_idc == 0)
    pVPS->profile_and_level[0].general_level_idc = CONCEAL_LEVEL_IDC;

  pVPS->vps_sub_layer_ordering_info_present_flag = u(pRP, 1);

  int layer_offset = pVPS->vps_sub_layer_ordering_info_present_flag ? 0 : pVPS->vps_max_sub_layers_minus1;

  for(int i = layer_offset; i <= pVPS->vps_max_sub_layers_minus1; ++i)
  {
    pVPS->vps_max_dec_pic_buffering_minus1[i] = ue(pRP);
    pVPS->vps_max_num_reorder_pics[i] = ue(pRP);
    pVPS->vps_max_latency_increase_plus1[i] = ue(pRP);
  }

  pVPS->vps_max_layer_id = u(pRP, 6);
  pVPS->vps_num_layer_sets_minus1 = ue(pRP);

  for(int i = 1; i <= pVPS->vps_num_layer_sets_minus1; ++i)
  {
    uint16_t uOffset = Min(i, 1);

    for(int j = 0; j <= pVPS->vps_max_layer_id; j++)
      pVPS->layer_id_included_flag[uOffset][j] = u(pRP, 1);
  }

  pVPS->vps_timing_info_present_flag = u(pRP, 1);

  if(pVPS->vps_timing_info_present_flag)
  {
    pVPS->vps_num_units_in_tick = u(pRP, 32);
    pVPS->vps_time_scale = u(pRP, 32);
    pVPS->vps_poc_proportional_to_timing_flag = u(pRP, 1);

    if(pVPS->vps_poc_proportional_to_timing_flag)
      pVPS->vps_num_ticks_poc_diff_one_minus1 = ue(pRP);

    pVPS->vps_num_hrd_parameters = ue(pRP);

    for(int i = 0; i < pVPS->vps_num_hrd_parameters; ++i)
    {
      uint16_t uOffset = Min(i, 1);
      pVPS->hrd_layer_set_idx[uOffset] = ue(pRP);

      if(uOffset)
        pVPS->cprms_present_flag[uOffset] = u(pRP, 1);
      else
        pVPS->cprms_present_flag[uOffset] = 1;
      hevc_hrd_parameters(&pVPS->hrd_parameter[uOffset], pVPS->cprms_present_flag[uOffset], pVPS->vps_max_sub_layers_minus1, pRP);
    }
  }

  if(u(pRP, 1)) // vps_extension_flag
  {
    while(more_rbsp_data(pRP))
      skip(pRP, 1); // vps_extension_data_flag
  }
  rbsp_trailing_bits(pRP);

  return AL_OK;
}

/*****************************************************************************/
static AL_PARSE_RESULT SeiActiveParameterSets(AL_TRbspParser* pRP, AL_THevcAup* aup, uint8_t* pSpsId)
{
  uint8_t active_video_parameter_set_id = u(pRP, 4);
  /*self_Contained_cvs_flag =*/ u(pRP, 1);
  /*no_parameter_set_update_flag =*/ u(pRP, 1);
  uint8_t num_sps_ids_minus1 = ue(pRP);
  COMPLY(num_sps_ids_minus1 < AL_HEVC_MAX_SPS);

  uint8_t active_seq_parameter_set_id[AL_HEVC_MAX_SPS];

  for(int i = 0; i <= num_sps_ids_minus1; ++i)
  {
    active_seq_parameter_set_id[i] = ue(pRP);
    COMPLY(active_seq_parameter_set_id[i] < AL_HEVC_MAX_SPS);
  }

  AL_THevcVps const* pVPS = &aup->pVPS[active_video_parameter_set_id];
  uint8_t MaxLayersMinus1 = Min(62, pVPS->vps_max_layers_minus1);

  for(int i = pVPS->vps_base_layer_internal_flag; i <= MaxLayersMinus1; ++i)
    /*layer_sps_idx[i] =*/ ue(pRP);

  COMPLY(aup->pSPS[active_seq_parameter_set_id[0]].bConceal == false);

  *pSpsId = active_seq_parameter_set_id[0];

  return AL_OK;
}

/*****************************************************************************/
static bool SeiPicTiming(AL_TRbspParser* pRP, AL_THevcSps* pSPS, AL_THevcPicTiming* pPicTiming)
{
  if(pSPS == NULL || pSPS->bConceal)
    return false;

  Rtos_Memset(pPicTiming, 0, sizeof(*pPicTiming));

  if(pSPS->vui_param.frame_field_info_present_flag)
  {
    pPicTiming->pic_struct = u(pRP, 4);
    pPicTiming->source_scan_type = u(pRP, 2);
    pPicTiming->duplicate_flag = u(pRP, 1);
    pSPS->sei_source_scan_type = pPicTiming->source_scan_type;
  }

  bool CpbDpbDelaysPresentFlag = pSPS->vui_param.hrd_param.nal_hrd_parameters_present_flag
                                 || pSPS->vui_param.hrd_param.vcl_hrd_parameters_present_flag;

  if(CpbDpbDelaysPresentFlag)
  {
    uint8_t syntax_size = pSPS->vui_param.hrd_param.au_cpb_removal_delay_length_minus1 + 1;
    pPicTiming->au_cpb_removal_delay_minus1 = u(pRP, syntax_size);

    syntax_size = pSPS->vui_param.hrd_param.dpb_output_delay_length_minus1 + 1;
    pPicTiming->pic_dpb_output_delay = u(pRP, syntax_size);

    if(pSPS->vui_param.hrd_param.sub_pic_hrd_params_present_flag)
    {
      syntax_size = pSPS->vui_param.hrd_param.dpb_output_delay_du_length_minus1 + 1;
      pPicTiming->pic_dpb_output_du_delay = u(pRP, syntax_size);

      if(pSPS->vui_param.hrd_param.sub_pic_cpb_params_in_pic_timing_sei_flag)
      {
        pPicTiming->num_decoding_units_minus1 = ue(pRP);
        pPicTiming->du_common_cpb_removal_delay_flag = u(pRP, 1);

        if(pPicTiming->du_common_cpb_removal_delay_flag)
        {
          syntax_size = pSPS->vui_param.hrd_param.du_cpb_removal_delay_increment_length_minus1 + 1;
          pPicTiming->du_common_cpb_removal_delay_increment_minus1 = u(pRP, syntax_size);
        }

        for(uint32_t i = 0; i <= pPicTiming->num_decoding_units_minus1; ++i)
        {
          /*pPicTiming->num_nalus_in_du_minus1[i] = */
          ue(pRP);

          if(!pPicTiming->du_common_cpb_removal_delay_flag && i < pPicTiming->num_decoding_units_minus1)
            /*pPicTiming->du_cpb_removal_delay_increment_minus1[i] = */ u(pRP, syntax_size);
        }
      }
    }
  }
  return true;
}

/*****************************************************************************/
static bool ParseSeiPayload(SeiParserParam* p, AL_TRbspParser* pRP, AL_ESeiPayloadType ePayloadType, int iPayloadSize, bool* bCanSendToUser, bool* bParsed)
{
  bool bParsingOk = true;
  AL_THevcAup* aup = &p->pIAup->hevcAup;
  *bCanSendToUser = true;
  *bParsed = true;
  switch(ePayloadType)
  {
  case SEI_PTYPE_PIC_TIMING: // picture_timing parsing
  {
    if(aup->pActiveSPS)
    {
      AL_THevcPicTiming tPictureTiming;
      bParsingOk = SeiPicTiming(pRP, aup->pActiveSPS, &tPictureTiming);

      if(bParsingOk)
        aup->ePicStruct = tPictureTiming.pic_struct;
    }
    else
      skip(pRP, iPayloadSize << 3);
    break;
  }
  case SEI_PTYPE_ACTIVE_PARAMETER_SETS:
  {
    uint8_t uSpsId;
    AL_PARSE_RESULT eResult = SeiActiveParameterSets(pRP, aup, &uSpsId);
    bParsingOk = eResult == AL_OK;

    if(bParsingOk)
      aup->pActiveSPS = &aup->pSPS[uSpsId];
    break;
  }
  default:
  {
    *bParsed = false;
    *bCanSendToUser = false;
    break;
  }
  }

  return bParsingOk;
}

/*****************************************************************************/
bool AL_HEVC_ParseSEI(AL_TAup* pIAup, AL_TRbspParser* pRP, bool bIsPrefix, AL_CB_ParsedSei* cb, AL_TSeiMetaData* pMeta)
{
  skipAllZerosAndTheNextByte(pRP);

  u(pRP, 16); // Skip NUT + temporal_id

  SeiParserParam tUserParam = { pIAup, bIsPrefix, cb, pMeta };
  SeiParserCB tSeiParserCb = { ParseSeiPayload, &tUserParam };

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
void AL_HEVC_GetCropInfo(AL_THevcSps const* pSPS, AL_TCropInfo* pCropInfo)
{
  if(pSPS->conformance_window_flag)
  {
    pCropInfo->bCropping = true;

    if(pSPS->chroma_format_idc == 1 || pSPS->chroma_format_idc == 2)
    {
      pCropInfo->uCropOffsetLeft = 2 * pSPS->conf_win_left_offset;
      pCropInfo->uCropOffsetRight = 2 * pSPS->conf_win_right_offset;
    }
    else
    {
      pCropInfo->uCropOffsetLeft = pSPS->conf_win_left_offset;
      pCropInfo->uCropOffsetRight = pSPS->conf_win_right_offset;
    }

    if(pSPS->chroma_format_idc == 1)
    {
      pCropInfo->uCropOffsetTop = 2 * pSPS->conf_win_top_offset;
      pCropInfo->uCropOffsetBottom = 2 * pSPS->conf_win_bottom_offset;
    }
    else
    {
      pCropInfo->uCropOffsetTop = pSPS->conf_win_top_offset;
      pCropInfo->uCropOffsetBottom = pSPS->conf_win_bottom_offset;
    }
  }
  else
  {
    ResetCropInfo(pCropInfo);
  }
}
