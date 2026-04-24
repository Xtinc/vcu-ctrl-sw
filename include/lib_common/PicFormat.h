// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/**************************************************************************//*!
   \addtogroup PicFormat

   Describes the format of a YUV buffer

   @{
   \file
******************************************************************************/
#pragma once

#include "lib_rtos/lib_rtos.h"

/*************************************************************************//*!
   \brief Struct for dimension
*****************************************************************************/
typedef struct AL_TDimension
{
  int32_t iWidth;
  int32_t iHeight;
}AL_TDimension;

/*************************************************************************//*!
   \brief Struct for position
*****************************************************************************/
typedef struct AL_TPosition
{
  int32_t iX;
  int32_t iY;
}AL_TPosition;

/*************************************************************************//*!
   \brief Struct for direction
*****************************************************************************/
typedef struct AL_TDirection
{
  int32_t iTop;
  int32_t iRight;
  int32_t iBottom;
  int32_t iLeft;
}AL_TDirection;

/*************************************************************************//*!
   \brief Struct for window
*****************************************************************************/
typedef struct AL_TWindow
{
  AL_TPosition tPos;
  AL_TDimension tDim;
}AL_TWindow;

/*************************************************************************//*!
   \brief Struct for dimension
*****************************************************************************/
typedef struct AL_TPhase
{
  uint8_t Horizontal;
  uint8_t Vertical;
}AL_TPhase;

/*************************************************************************//*!
   \brief Cropping Info on the YUV reconstructed
 *************************************************************************/
typedef struct AL_TCropInfo
{
  bool bCropping;         /*!< Cropping information present flag    */
  uint32_t uCropOffsetLeft;   /*!< Left   offset of the cropping window */
  uint32_t uCropOffsetRight;  /*!< Right  offset of the cropping window */
  uint32_t uCropOffsetTop;    /*!< Top    offset of the cropping window */
  uint32_t uCropOffsetBottom; /*!< Bottom offset of the cropping window */
}AL_TCropInfo;

/****************************************************************************/
static inline void ResetCropInfo(AL_TCropInfo* pCropInfo)
{
  pCropInfo->bCropping = false;
  pCropInfo->uCropOffsetLeft = 0;
  pCropInfo->uCropOffsetRight = 0;
  pCropInfo->uCropOffsetTop = 0;
  pCropInfo->uCropOffsetBottom = 0;
}

/*************************************************************************//*!
   \brief Chroma mode. Describes how many chroma samples we have relatively
   to the luma samples.
*****************************************************************************/
typedef enum AL_EChromaMode
{
  AL_CHROMA_MONO = 0, /*!< Monochrome */
  AL_CHROMA_4_0_0 = AL_CHROMA_MONO, /*!< 4:0:0 = Monochrome */
  AL_CHROMA_4_2_0 = 1, /*!< 4:2:0 chroma sampling */
  AL_CHROMA_4_2_2 = 2, /*!< 4:2:2 chroma sampling */
  AL_CHROMA_4_4_4 = 3, /*!< 4:4:4 chroma sampling */
  AL_CHROMA_MAX_ENUM, /* sentinel */
}AL_EChromaMode;

/*************************************************************************//*!
   \brief Frame buffer storage mode. It describes the scan order of the
   samples inside the frame buffer.
*****************************************************************************/
typedef enum AL_EFbStorageMode
{
  AL_FB_RASTER,     /*!< Samples are stored in raster scan order */
  AL_FB_TILE_32x4,  /*!< Samples are stored going raster inside 4x4 blocks, themselves inside 32x4 tiles */
  AL_FB_TILE_64x4,  /*!< Samples are stored going raster inside 4x4 blocks, themselves inside 64x4 tiles */
  AL_FB_MAX_ENUM, /* sentinel */
}AL_EFbStorageMode;

/*************************************************************************//*!
   \brief Frame buffer plane mode. A plane is a contiguous memory chunk that
   can contain one or more components of the frame buffer. Components can be
   for instance Y/U/V, or R/G/B.
*****************************************************************************/
typedef enum AL_EPlaneMode
{
  AL_PLANE_MODE_PLANAR, /*!< Each component is stored its own separated plane */
  AL_PLANE_MODE_MONOPLANE = AL_PLANE_MODE_PLANAR,  /*!< There is only one component, and thus only one plane  */
  AL_PLANE_MODE_SEMIPLANAR, /*!< There is one plane for the luma component, and one plane for interleaved chroma-U and chroma-V components */
  AL_PLANE_MODE_INTERLEAVED, /*!< All components are stored in a single unique plane, in an interleaved fashion */
  AL_PLANE_MODE_MAX_ENUM, /* sentinel */
}AL_EPlaneMode;

/*************************************************************************//*!
   \brief Frame buffer component order. In case of a planar frame buffer, as
   each component is stored in a different plane, it will describe the order
   of the planes. In case of a semiplanar or interleaved frame buffer, it
   will describe the order of component inside a plane.
*****************************************************************************/
typedef enum AL_EComponentOrder
{
  AL_COMPONENT_ORDER_YUV,
  AL_COMPONENT_ORDER_YVU,
  AL_COMPONENT_ORDER_UYV,
  AL_COMPONENT_ORDER_UVY,
  AL_COMPONENT_ORDER_VYU,
  AL_COMPONENT_ORDER_VUY,
  AL_COMPONENT_ORDER_YUYV,
  AL_COMPONENT_ORDER_UYVY,
  AL_COMPONENT_ORDER_SKIP_OFFSET,
  AL_COMPONENT_ORDER_RGB = AL_COMPONENT_ORDER_SKIP_OFFSET,
  AL_COMPONENT_ORDER_RBG,
  AL_COMPONENT_ORDER_GRB,
  AL_COMPONENT_ORDER_GBR,
  AL_COMPONENT_ORDER_BRG,
  AL_COMPONENT_ORDER_BGR,
  AL_COMPONENT_ORDER_MAX_ENUM, /* sentinel */
}AL_EComponentOrder;

/*************************************************************************//*!
   \brief Frame buffer sample pack mode. Describes on how many bits each sample
   is stored.
*****************************************************************************/
typedef enum AL_ESamplePackMode
{
  AL_SAMPLE_PACK_MODE_BYTE, /*!< 8 bits samples stored on 8 bits, 10/12 on 16 bits */
  AL_SAMPLE_PACK_MODE_PACKED, /*!< n bits samples stored exactly on n bits */
  AL_SAMPLE_PACK_MODE_PACKED_XV, /*!< 3x10 bits samples stored on 32 bits */
  AL_SAMPLE_PACK_MODE_MAX_ENUM, /* sentinel */
}AL_ESamplePackMode;

/*************************************************************************//*!
  \brief Frame buffer alpha mode. Describes if buffer contains alpha information,
  and if so, describes its position relatively to other samples.
*************************************************************************/
typedef enum AL_EAlphaMode
{
  AL_ALPHA_MODE_DISABLED, /*!< No alpha */
  AL_ALPHA_MODE_BEFORE, /*!< Alpha is stored before other components */
  AL_ALPHA_MODE_AFTER, /*!< Alpha is stored after other components */
  AL_ALPHA_MODE_MAX_ENUM, /* sentinel */
}AL_EAlphaMode;

/*************************************************************************//*!
   \brief Describes the format of a YUV buffer
*****************************************************************************/
typedef struct AL_TPicFormat
{
  AL_EChromaMode eChromaMode;
  AL_EAlphaMode eAlphaMode;
  uint8_t uBitDepth;
  AL_EFbStorageMode eStorageMode;
  AL_EPlaneMode ePlaneMode;
  AL_EComponentOrder eComponentOrder;
  AL_ESamplePackMode eSamplePackMode;
  bool bCompressed;
  bool bMSB;
}AL_TPicFormat;

/*************************************************************************//*!
   \brief Output type
 *************************************************************************/
typedef enum AL_EOutputType
{
  AL_OUTPUT_PRIMARY,
  AL_OUTPUT_MAIN,
  AL_OUTPUT_POSTPROC,
  AL_OUTPUT_LCEVC,
  AL_OUTPUT_MAX_ENUM,
}AL_EOutputType;

/****************************************************************************/
AL_EPlaneMode GetInternalBufPlaneMode(AL_EChromaMode eChromaMode);

/****************************************************************************/
AL_ESamplePackMode GetInternalBufSamplePackMode(AL_EFbStorageMode eFbStorageMode, uint8_t uBitDepth);

/****************************************************************************/
int GetTileWidth(AL_EFbStorageMode eMode, uint8_t uBitDepth);

/****************************************************************************/
int GetTileHeight(AL_EFbStorageMode eMode);

/****************************************************************************/
int GetTileSize(AL_EFbStorageMode eMode, uint8_t uBitDepth);

/****************************************************************************/
bool IsRgbComponentOrder(AL_EComponentOrder eComponentOrder);

/****************************************************************************/
AL_TPicFormat GetDefaultPicFormat(void);

/*****************************************************************************/
bool IsTile(AL_EFbStorageMode eStorageMode);

/*@}*/
