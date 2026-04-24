// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <climits>
#include <string>
#include <set>
#include "lib_app/utils.h"

extern "C"
{
#include "lib_common/FourCC.h"
#include "lib_decode/DecSettings.h"
}

using namespace std;

/******************************************************************************/
enum EDecErrorLevel
{
  DEC_WARNING,
  DEC_ERROR,
};

/******************************************************************************/
static int const zDefaultInputBufferSize = 32 * 1024;
static const int OUTPUT_BD_FIRST = 0;
static const int OUTPUT_BD_ALLOC = -1;
static const int OUTPUT_BD_STREAM = -2;
static const int SEI_NOT_ASSOCIATED_WITH_FRAME = -1;
static uint32_t constexpr uDefaultNumBuffersHeldByNextComponent = 1; /* We need at least 1 buffer to copy the output on a file */

static const int DEFAULT_DEC_APB_ID = 2;

/******************************************************************************/
struct Config
{
  Config();

  bool help = false;

  string sIn;
  string sMainOut = ""; // Output rec file
  string sCrc;

  AL_TDecSettings tDecSettings {};
  AL_TDecOutputSettings tUserOutputSettings {};
  bool bEnableCrop = false;

  AL_EDeviceType iDeviceType = AL_DEVICE_TYPE_BOARD; // board
  AL_ESchedulerType iSchedulerType = AL_SCHEDULER_TYPE_MCU;
  bool bSelectDeviceWithLowestAvailableResources = false;
  int iOutputBitDepth = OUTPUT_BD_ALLOC;
  TFourCC tOutputFourCC = FOURCC(NULL);
  int iTraceIdx = -1;
  int iTraceNumber = 0;
  bool bForceCleanBuffers = false;
  bool bEnableYUVOutput = true;
  unsigned int uInputBufferNum = 2;
  size_t zInputBufferSize = zDefaultInputBufferSize;
  AL_EIpCtrlMode ipCtrlMode = AL_IPCTRL_MODE_STANDARD;
  string md5File = "";
  string apbFile = "";
  string sSplitSizesFile = "";
  bool trackDma = false;
  int hangers = 0;
  int iLoop = 1;
  bool bMultiChunk = false;
  bool bCertCRC = false;
  std::set<std::string> sDecDevicePath;
  int iTimeoutInSeconds = -1;
  int iMaxFrames = INT_MAX;
  string seiFile = "";
  string hdrFile = "";
  bool bUsePreAlloc = false;

  bool UseBaseDecoder() const { return true; }

  EDecErrorLevel eExitCondition = DEC_ERROR;
};

/******************************************************************************/
Config ParseCommandLine(int argc, char* argv[]);
AL_EFbStorageMode GetMainOutputStorageMode(AL_TDecOutputSettings tUserOutputSettings, AL_EFbStorageMode eOutstorageMode);
bool IsOutputStorageModeCompressed(AL_TDecOutputSettings tUserOutputSettings, bool bMainOutputCompressed);
