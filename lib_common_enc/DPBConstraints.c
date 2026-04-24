// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "DPBConstraints.h"
#include "lib_rtos/message.h"

/****************************************************************************/
uint8_t AL_DPBConstraint_GetMaxRef_DefaultGop(const AL_TGopParam* pGopParam, AL_ECodec eCodec)
{
  (void)eCodec;

  /*
   * The structure of the default gop makes it that:
   *
   * When there is no reordering (no B frames) We only need one reference buffer
   * at a time
   *  ,-,,-,,-,
   * v  |v |v |
   * I  P  P  P ....
   *
   * When there are B frames, we need two reference buffer at a time
   *
   *   -
   * ,   `--
   * v   | v
   * I B B P B B ...
   * 0 2 3 1
   */

  uint8_t uNumRef = 1;

  if(pGopParam->uNumB > 0 || (pGopParam->eMode & AL_GOP_FLAG_B_ONLY))
    ++uNumRef;

  /*
  * We need an extra buffer to keep the long term picture while we deal
  * with the other pictures as normal
  */
  if(pGopParam->bEnableLT)
    ++uNumRef;

  return uNumRef;
}

#define NEXT_PYR_LEVEL_NUMB(uNumBForPyrLevel) (((uNumBForPyrLevel) << 1) + 1)
uint8_t AL_DPBConstraint_GetMaxRef_PyramidalGop(const AL_TGopParam* pGopParam, AL_ECodec eCodec)
{
  (void)eCodec;

  /*
   * Number of references required is dependent of the height of the pyramid.
   * This is the number of B-Reference-Levels + 1 for I and 1 for P.
   * The higher number of references reached occurs when encoding the bottom-left
   * non-ref b.
   *
   * I               P
   *         B
   *     B       B
   *   b   b   b   b
   *   ^
   *   |
   * Worst-Case
   */

  uint8_t uNumRef = 1;

  if(pGopParam->uNumB != 0)
  {
    uint8_t uPyrLevel = 0;
    uint8_t uNumBForPyrLevel = 1;
    uint8_t uNumBForNextPyrLevel = NEXT_PYR_LEVEL_NUMB(uNumBForPyrLevel);

    while(pGopParam->uNumB >= uNumBForNextPyrLevel)
    {
      uNumBForPyrLevel = uNumBForNextPyrLevel;
      uNumBForNextPyrLevel = NEXT_PYR_LEVEL_NUMB(uNumBForPyrLevel);
      uPyrLevel++;
    }

    uNumRef = uPyrLevel + 2;

    /* Add 1 in AVC for the current frame */
    if(eCodec == AL_CODEC_AVC)
      uNumRef++;
  }

  if(pGopParam->bEnableLT)
    uNumRef++;

  return uNumRef;
}

/****************************************************************************/
uint8_t AL_DPBConstraint_GetMaxRef_LowDelayGop(const AL_TGopParam* pGopParam, AL_ECodec eCodec)
{

  /* Ref of a P picture */
  uint8_t uNumRef = 1;

  if(pGopParam->eMode == AL_GOP_MODE_LOW_DELAY_B)
  {
    /* Refs of a B picture */
    uNumRef++;

    /*
     * Add 1 in AVC for the current frame. If GOP length is 1, the current frame ref can be spared as
     * the ref to remove is always the oldest ref
     */
    if(eCodec == AL_CODEC_AVC && pGopParam->uGopLength > 1)
      uNumRef++;
  }

  if(pGopParam->bEnableLT)
    ++uNumRef;

  return uNumRef;
}

/****************************************************************************/
static uint8_t AL_DPBConstraint_AdjustMaxRefInterlaced(AL_ECodec eCodec, uint8_t uProgressiveMaxRef)
{
  uint8_t uInterleavedMaxRef;

  if(eCodec == AL_CODEC_AVC)
  {
    // In AVC, top and bottom fields holds in a single reference. Yet, we still need to
    // add a reference to keep the current buffer as reference when dealing with the
    // second field.
    uInterleavedMaxRef = uProgressiveMaxRef + 1;
  }
  else
  {
    // In HEVC, each field is handled as a separated reference.
    uInterleavedMaxRef = 2 * uProgressiveMaxRef;
  }

  return uInterleavedMaxRef;
}

/****************************************************************************/
uint8_t AL_DPBConstraint_GetMaxRef(const AL_TGopParam* pGopParam, AL_ECodec eCodec, AL_EVideoMode eVideoMode, uint8_t uLookAheadAdditionalRef)
{
  (void)eVideoMode;

  uint8_t uMaxRef = 0;

  if((pGopParam->eMode & AL_GOP_FLAG_DEFAULT) || (pGopParam->eMode == AL_GOP_MODE_ADAPTIVE))
    uMaxRef = AL_DPBConstraint_GetMaxRef_DefaultGop(pGopParam, eCodec);
  else if(pGopParam->eMode & AL_GOP_FLAG_PYRAMIDAL)
    uMaxRef = AL_DPBConstraint_GetMaxRef_PyramidalGop(pGopParam, eCodec);
  else if(pGopParam->eMode & AL_GOP_FLAG_LOW_DELAY)
    uMaxRef = AL_DPBConstraint_GetMaxRef_LowDelayGop(pGopParam, eCodec);
  else
  {
    DBG_ASSERT_COND(0);
  }

  if(eVideoMode != AL_VM_PROGRESSIVE)
    uMaxRef = AL_DPBConstraint_AdjustMaxRefInterlaced(eCodec, uMaxRef);

  uMaxRef += uLookAheadAdditionalRef;

  return uMaxRef;
}

/****************************************************************************/
uint8_t AL_DPBConstraint_GetMaxDPBSize(const AL_TEncChanParam* pChParam)
{
  uint8_t uLookAheadAdditionalRef = 0;

  AL_ECodec eCodec = AL_GET_CODEC(pChParam->eProfile);
  uint8_t uMaxDPBSize = AL_DPBConstraint_GetMaxRef(&pChParam->tGopParam, eCodec, pChParam->eVideoMode, uLookAheadAdditionalRef);

  if(eCodec == AL_CODEC_HEVC)
  {
    /* Reconstructed buffer is an actual part of the dpb algorithm in hevc */
    uMaxDPBSize++;
  }

  return uMaxDPBSize;
}
