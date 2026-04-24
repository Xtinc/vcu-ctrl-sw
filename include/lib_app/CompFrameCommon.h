// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

extern "C"
{
#include "lib_common/PicFormat.h"
}
#include <fstream>
#include <stdexcept>

static constexpr uint8_t CurrentCompFileVersion = 3;

enum ETileMode : uint8_t
{
  TILE_64x4_v0 = 0,
  TILE_64x4_v1 = 1,
  TILE_32x4_v1 = 2,
  RASTER = 5,
  TILE_MAX_ENUM,
};

ETileMode EFbStorageModeToETileMode(AL_EFbStorageMode eFbStorageMode, uint8_t uBitDepth);
AL_EFbStorageMode ETileModeToEFbStorageMode(ETileMode eTileMode);
