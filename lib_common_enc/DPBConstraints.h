// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_common_enc/EncChanParam.h"

/*************************************************************************//*!
   \brief Get the maximum number of reference buffers in the DPB
   \param[in] pGopParam Pointer to the gop parameters
   \param[in] eCodec Codec
   \param[in] eVideoMode Video Mode
   \param[in] uLookAheadAdditionalRef Add optional references required for lookahead features
   \return The maximum number of references
*****************************************************************************/
uint8_t AL_DPBConstraint_GetMaxRef(const AL_TGopParam* pGopParam, AL_ECodec eCodec, AL_EVideoMode eVideoMode, uint8_t uLookAheadAdditionalRef);

/*************************************************************************//*!
   \brief Get the maximum size of the dpb required for the encoding parameters
   provided
   \param[in] pChParam Pointer to the channel parameters
   \return The maximum size of the DPB
*****************************************************************************/
uint8_t AL_DPBConstraint_GetMaxDPBSize(const AL_TEncChanParam* pChParam);
