// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "ChannelResources.h"
#include "Utils.h"
#include "lib_rtos/lib_rtos.h"

static int divideRoundUp(AL_64U dividende, AL_64U divisor)
{
  return (dividende + divisor - 1) / divisor;
}

static int GetMinCoresCount(int width, int maxWidth)
{
  return divideRoundUp(width, maxWidth);
}

int GetCoreResources(int coreFrequency, int margin)
{
  return coreFrequency - (coreFrequency / 100) * margin;
}

static int ChoseCoresCount(int width, int height, int frameRate, int clockRatio, int resourcesByCore, int maxWidth, int cycles32x32)
{
  AL_64U channelResources = AL_GetResources(width, height, frameRate, clockRatio, cycles32x32);
  return Max(GetMinCoresCount(width, maxWidth), divideRoundUp(channelResources, resourcesByCore));
}

void AL_CoreConstraint_Init(AL_CoreConstraint* constraint, int coreFrequency, int margin, int const* hardwareCyclesCounts, int minWidth, int maxWidth, int lcuSize)
{
  constraint->minWidth = minWidth;
  constraint->maxWidth = maxWidth;
  constraint->lcuSize = lcuSize;
  constraint->resources = GetCoreResources(coreFrequency, margin);

  for(int i = 0; i < 4; ++i)
    constraint->cycles32x32[i] = hardwareCyclesCounts[i];

  constraint->enableMultiCore = true;
}

int AL_CoreConstraint_GetExpectedNumberOfCores(AL_CoreConstraint* constraint, int width, int height, int chromaModeIdc, int frameRate, int clockRatio)
{
  return ChoseCoresCount(width, height, frameRate, clockRatio, constraint->resources, constraint->maxWidth, constraint->cycles32x32[chromaModeIdc]);
}

int AL_CoreConstraint_GetMinCoresCount(AL_CoreConstraint* constraint, int width)
{
  return GetMinCoresCount(width, constraint->maxWidth);
}

static int getLcuCount(int width, int height)
{
  /* Fixed LCU Size chosen for resources calculus */
  int const lcuPicHeight = 32;
  int const lcuPicWidth = 32;
  return divideRoundUp(width, lcuPicWidth) * divideRoundUp(height, lcuPicHeight);
}

AL_64U AL_GetResources(int width, int height, int frameRate, int clockRatio, int cycles32x32)
{
  if(clockRatio == 0)
    return 0;

  AL_64U lcuCount = getLcuCount(width, height);
  AL_64U dividende = lcuCount * (AL_64U)frameRate;
  AL_64U divisor = (AL_64U)clockRatio;
  return divideRoundUp(dividende, divisor) * (AL_64U)cycles32x32;
}

static int ToCtb(int val, int ctbSize)
{
  return divideRoundUp(val, ctbSize);
}

bool AL_Constraint_NumTileIsSane(AL_ECodec codec, int width, int numTile, int log2MaxCuSize, AL_NumCoreDiagnostic* diagnostic)
{
  (void)codec;
  /*
   * Hardware limitation, for each core, we need at least:
   * -> 3 CTB for VP9 / HEVC64
   * -> 4 CTB for HEVC32
   * -> 5 MB for AVC
   * Each core starts on a tile.
   * Tiles are aligned on 64 bytes.
   * For JPEG, each core works on a different frame.
   */

  int tilePerFrame = numTile;

  int ctbSize = 1 << log2MaxCuSize;
  int const MIN_CTB_PER_TILE = 9 - log2MaxCuSize;
  int widthPerTileInCtb = ToCtb(width / tilePerFrame, ctbSize);

  int offset = 0;
  int roundedOffset = 0; // A core needs to starts at a 64 bytes aligned offset

  if(diagnostic)
    Rtos_Memset(diagnostic, 0, sizeof(*diagnostic));

  for(int tile = 0; tile < tilePerFrame; ++tile)
  {
    offset = roundedOffset;
    int curTileMinWidthInCtb = MIN_CTB_PER_TILE * ctbSize;
    offset += curTileMinWidthInCtb;
    roundedOffset = RoundUp(offset, 64);
  }

  if(diagnostic)
  {
    diagnostic->requiredWidthInCtbPerCore = MIN_CTB_PER_TILE;
    diagnostic->actualWidthInCtbPerCore = widthPerTileInCtb;
  }

  return widthPerTileInCtb >= MIN_CTB_PER_TILE && offset <= RoundUp(width, ctbSize);
}
