// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "lib_common/Planes.h"
#include "stdio.h"
#include <stdlib.h>

bool AL_Plane_IsPixelPlane(AL_EPlaneId ePlaneId)
{
  return ePlaneId < (int)AL_PLANE_MAP_Y;
}

bool AL_Plane_IsMapPlane(AL_EPlaneId ePlaneId)
{
  return ePlaneId != AL_PLANE_MAX_ENUM && !AL_Plane_IsPixelPlane(ePlaneId);
}

static void AddPlane(AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES], int* iNbPlanes, AL_EPlaneId ePlaneType)
{
  usedPlanes[*iNbPlanes] = ePlaneType;
  (*iNbPlanes)++;
}

int AL_Plane_GetBufferPixelPlanes(AL_TPicFormat tPicFormat, AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES])
{
  int iNbPlanes = 0;

  if(AL_PLANE_MODE_INTERLEAVED == tPicFormat.ePlaneMode && AL_CHROMA_MONO != tPicFormat.eChromaMode)
  {
    AddPlane(usedPlanes, &iNbPlanes, AL_PLANE_YUV);
    return iNbPlanes;
  }

  AddPlane(usedPlanes, &iNbPlanes, AL_PLANE_Y);

  if(AL_PLANE_MODE_SEMIPLANAR == tPicFormat.ePlaneMode)
    AddPlane(usedPlanes, &iNbPlanes, AL_PLANE_UV);
  else if(AL_CHROMA_4_0_0 != tPicFormat.eChromaMode)
  {
    AddPlane(usedPlanes, &iNbPlanes, AL_PLANE_U);
    AddPlane(usedPlanes, &iNbPlanes, AL_PLANE_V);
  }

  return iNbPlanes;
}

static void AddBufferMapPlanes(AL_TPicFormat tPicFormat, AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES], int* pNbPlanes)
{
  AddPlane(usedPlanes, pNbPlanes, AL_PLANE_MAP_Y);

  if(AL_CHROMA_MONO == tPicFormat.eChromaMode)
    return;

  if(AL_PLANE_MODE_SEMIPLANAR == tPicFormat.ePlaneMode)
    AddPlane(usedPlanes, pNbPlanes, AL_PLANE_MAP_UV);
  else if(AL_PLANE_MODE_PLANAR == tPicFormat.ePlaneMode)
  {
    AddPlane(usedPlanes, pNbPlanes, AL_PLANE_MAP_U);
    AddPlane(usedPlanes, pNbPlanes, AL_PLANE_MAP_V);
  }
}

int AL_Plane_GetBufferMapPlanes(AL_TPicFormat tPicFormat, AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES])
{
  int iNbPlanes = 0;
  AddBufferMapPlanes(tPicFormat, usedPlanes, &iNbPlanes);
  return iNbPlanes;
}

int AL_Plane_GetBufferPlanes(AL_TPicFormat tPicFormat, AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES])
{
  if(AL_PLANE_MODE_MAX_ENUM == tPicFormat.ePlaneMode)
    tPicFormat.ePlaneMode = GetInternalBufPlaneMode(tPicFormat.eChromaMode);

  int iNbPlanes = AL_Plane_GetBufferPixelPlanes(tPicFormat, usedPlanes);

  if(tPicFormat.bCompressed)
    AddBufferMapPlanes(tPicFormat, usedPlanes, &iNbPlanes);
  return iNbPlanes;
}

bool AL_Plane_Exists(AL_EPlaneMode ePlaneMode, bool bIsCompressed, AL_EPlaneId ePlaneId)
{
  if(AL_Plane_IsMapPlane(ePlaneId) && !bIsCompressed)
    return false;
  switch(ePlaneId)
  {
  case AL_PLANE_Y:
  case AL_PLANE_MAP_Y:
    return true;
  case AL_PLANE_UV:
  case AL_PLANE_MAP_UV:
    return AL_PLANE_MODE_SEMIPLANAR == ePlaneMode;
  case AL_PLANE_U:
  case AL_PLANE_V:
  case AL_PLANE_MAP_U:
  case AL_PLANE_MAP_V:
    return AL_PLANE_MODE_PLANAR == ePlaneMode;
  case AL_PLANE_YUV:
    return (AL_PLANE_MODE_INTERLEAVED == ePlaneMode) && !bIsCompressed;
  default:
    return false;
  }

  return false;
}
