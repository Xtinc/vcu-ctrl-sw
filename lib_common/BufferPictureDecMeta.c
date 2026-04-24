// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "lib_common/BufferPictureDecMeta.h"
#include "lib_rtos/lib_rtos.h"

static bool PictureDecMeta_Destroy(AL_TMetaData* pMeta)
{
  AL_TPictureDecMetaData* pPictureMeta = (AL_TPictureDecMetaData*)pMeta;
  Rtos_Free(pPictureMeta);
  return true;
}

AL_TPictureDecMetaData* AL_PictureDecMetaData_Clone(AL_TPictureDecMetaData* pMeta)
{
  if(!pMeta)
    return NULL;

  AL_TPictureDecMetaData* pPictureMeta = AL_PictureDecMetaData_Create();

  if(!pPictureMeta)
    return NULL;

  pPictureMeta->bLastFrameFromInputPayload = pMeta->bLastFrameFromInputPayload;

  return pPictureMeta;
}

static AL_TMetaData* PictureDecMeta_Clone(AL_TMetaData* pMeta)
{
  return (AL_TMetaData*)AL_PictureDecMetaData_Clone((AL_TPictureDecMetaData*)pMeta);
}

AL_TPictureDecMetaData* AL_PictureDecMetaData_Create(void)
{
  AL_TPictureDecMetaData* pMeta = (AL_TPictureDecMetaData*)Rtos_Malloc(sizeof(*pMeta));

  if(!pMeta)
    return NULL;

  pMeta->tMeta.eType = AL_META_TYPE_PICTURE_DECODE;
  pMeta->tMeta.MetaDestroy = PictureDecMeta_Destroy;
  pMeta->tMeta.MetaClone = PictureDecMeta_Clone;

  pMeta->bLastFrameFromInputPayload = false;
  return pMeta;
}
