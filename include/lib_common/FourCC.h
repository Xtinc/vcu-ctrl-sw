// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/**************************************************************************//*!
   \addtogroup FourCC

   This regroups all the functions that can be used to know how the framebuffer
   is stored in memory

   @{
   \file
******************************************************************************/
#pragma once

#include "lib_rtos/lib_rtos.h"
#include "lib_common/PicFormat.h"

/*************************************************************************//*!
   \brief FOURCC identifier type
*****************************************************************************/
typedef uint32_t TFourCC;

#define FOURCC(A) ((TFourCC)(((uint32_t)((# A)[0])) \
                             | ((uint32_t)((# A)[1]) << 8) \
                             | ((uint32_t)((# A)[2]) << 16) \
                             | ((uint32_t)((# A)[3]) << 24)))

/*************************************************************************//*!
   \brief Returns the ChromaMode identifier according to the tFourCC parameter
   \param[in] tFourCC FourCC format of the current picture
   \return return the ChomaMode according to the tFourCC parameter
*****************************************************************************/
AL_EChromaMode AL_GetChromaMode(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns the AlphaMode identifier according to the tFourCC parameter
   \param[in] tFourCC FourCC format of the current picture
   \return return the AlphaMode according to the tFourCC parameter
*****************************************************************************/
AL_EAlphaMode AL_GetAlphaMode(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns the PlaneMode identifier according to the tFourCC parameter
   \param[in] tFourCC FourCC format of the current picture
   \return return the PlaneMode according to the tFourCC parameter
*****************************************************************************/
AL_EPlaneMode AL_GetPlaneMode(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns the PlaneOrder identifier according to the tFourCC parameter
   \param[in] tFourCC FourCC format of the current picture
   \return return the PlaneOrder according to the tFourCC parameter
*****************************************************************************/
AL_EComponentOrder AL_GetPlaneOrder(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns the samplePackMode identifier according to the tFourCC parameter
   \param[in] tFourCC FourCC format of the current picture
   \return return the samplePackMode according to the tFourCC parameter
*****************************************************************************/
AL_ESamplePackMode AL_GetSamplePackMode(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns the bitDepth according to the tFourCC parameter
   \param[in] tFourCC FourCC format of the current picture
   \return return the bitDepth according to the tFourCC parameter
*****************************************************************************/
uint8_t AL_GetBitDepth(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns the number of byte per color sample according to the tFourCC parameter
   \param[in] tFourCC FourCC format of the current picture
   \return number of bytes per color sample according to the tFourCC parameter
*****************************************************************************/
int AL_GetPixelSize(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns the chroma subsampling according to the tFourCC parameter
   \param[in] tFourCC fourCC format of the current picture
   \param[out] sx subsampling x
   \param[out] sy subsampling y
*****************************************************************************/
void AL_GetSubsampling(TFourCC tFourCC, int* sx, int* sy);

/*************************************************************************//*!
   \brief Returns true if YUV format specified by tFourCC is monochrome
   \param[in] tFourCC FourCC format of the current picture
   \return true if YUV is monochrome according to the tFourCC parameter
*****************************************************************************/
bool AL_IsMonochrome(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns true if YUV format specified by tFourCC is semiplanar
   \param[in] tFourCC FourCC format of the current picture
   \return true if YUV is semiplanar according to the tFourCC parameter
*****************************************************************************/
bool AL_IsSemiPlanar(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns true if YUV format specified by tFourCC is 444 interleaved
   \param[in] tFourCC FourCC format of the current picture
   \return true if YUV is interleaved according to the tFourCC parameter
*****************************************************************************/
bool AL_IsInterleaved(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns true if YUV format specified by tFourCC is tiled
   \param[in] tFourCC FourCC format of the current picture
   \return true if YUV is tiled according to the tFourCC parameter
*****************************************************************************/
bool AL_IsTiled(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns true if YUV format specified by tFourCC is  non-compressed tile
   \param[in] tFourCC FourCC format of the current picture
   \return true if YUV is non compressed tiled according to the tFourCC parameter
*****************************************************************************/
bool AL_IsNonCompTiled(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns the storage buffer format specified by tFourCC
   \param[in] tFourCC FourCC format of the current picture
   \return the storage buffer format specified by tFourCC
*****************************************************************************/
AL_EFbStorageMode AL_GetStorageMode(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns true if tFourCC specifies that the yuv buffer is compressed
   \param[in] tFourCC FourCC format of the current picture
   \return return true tFourCC indicates buffer compression enabled
*****************************************************************************/
bool AL_IsCompressed(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns true if tFourCC specifies that the yuv buffer is 10 bit packed
   \param[in] tFourCC FourCC format of the current picture
   \return return true tFourCC indicates that buffer is 10bit packed
*****************************************************************************/
bool AL_Is10bPacked(TFourCC tFourCC);

/*************************************************************************//*!
   \brief Returns FourCC from AL_TPicFormat
   \param[in] tPicFormat the pict format
   \return return corresponding FourCC, 0 if picFormat does not exist
*****************************************************************************/
TFourCC AL_GetFourCC(AL_TPicFormat tPicFormat);

/*************************************************************************//*!
   \brief Returns pointer to AL_TPicFormat from FourCC
   \param[in] tFourCC the FourCC
   \param[out] tPicFormat corresponding PicFormat if FourCC exists, untouched otherwise
   \return return true if FourCC exists, false otherwise
*****************************************************************************/
bool AL_GetPicFormat(TFourCC tFourCC, AL_TPicFormat* tPicFormat);

/*@}*/
