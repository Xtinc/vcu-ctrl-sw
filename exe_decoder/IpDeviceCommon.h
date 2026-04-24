// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_app/utils.h"

#include <string>

extern "C"
{
#include "lib_fpga/DmaAlloc.h"
#include "lib_common_dec/DecoderTraceHook.h"
}

/*****************************************************************************/
typedef struct AL_TAllocator AL_TAllocator;
typedef struct AL_TIpCtrl AL_TIpCtrl;
typedef struct AL_TDriver AL_TDriver;

/*****************************************************************************/
struct CIpDeviceParam
{
  int iDeviceType;
  int iSchedulerType;
  bool bTrackDma = false;
  uint8_t uNumCore = 0;
  int iHangers = 0;
  AL_EIpCtrlMode ipCtrlMode;
  std::string apbFile;
  bool bSelectDeviceWithLowestAvailableResources;
};

struct I_IpDevice
{
  virtual ~I_IpDevice() = default;
  virtual void* GetScheduler() = 0;
  virtual AL_TAllocator* GetAllocator() = 0;
};

/*****************************************************************************/
AL_TAllocator* CreateDmaAllocator(const char* deviceName);
