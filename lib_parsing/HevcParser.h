// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once
#include "Aup.h"
#include "common_syntax.h"
#include "Concealment.h"

#include "lib_common/PPS.h"
#include "lib_common/BufferSeiMeta.h"
#include "lib_common_dec/RbspParser.h"
#include "lib_common_dec/DecCallbacks.h" // for AL_CB_ParsedSEI

void AL_HEVC_InitAUP(AL_THevcAup* pAUP);
AL_PARSE_RESULT AL_HEVC_ParsePPS(AL_TAup* pIAup, AL_TRbspParser* pRP, uint16_t* pPpsId);
AL_PARSE_RESULT AL_HEVC_ParseSPS(AL_TRbspParser* pRP, AL_THevcSps* pSPS);
AL_PARSE_RESULT AL_HEVC_ParseVPS(AL_TAup* pIAup, AL_TRbspParser* pRP);
bool AL_HEVC_ParseSEI(AL_TAup* pIAup, AL_TRbspParser* pRP, bool bIsPrefix, AL_CB_ParsedSei* cb, AL_TSeiMetaData* pMeta);
void AL_HEVC_GetCropInfo(AL_THevcSps const* pSPS, AL_TCropInfo* pCropInfo);

/*************************************************************************//*!
   \brief the short term reference picture computation
   \param[out] pSPS   Pointer to the SPS structure containing the ref_pic_set structure and variables
   \param[in]  RpsIdx Idx of the current ref_pic_set
   \param[in]  pRP    Pointer to NAL parser
*****************************************************************************/
bool AL_HEVC_short_term_ref_pic_set(AL_THevcSps* pSPS, uint8_t RpsIdx, AL_TRbspParser* pRP);
