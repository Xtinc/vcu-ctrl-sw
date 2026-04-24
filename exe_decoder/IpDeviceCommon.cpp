// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include <stdexcept>
#include <string>
#include "IpDeviceCommon.h"

AL_TAllocator* CreateDmaAllocator(const char* deviceName)
{
  auto h = AL_DmaAlloc_Create(deviceName);

  if(h == nullptr)
    throw std::runtime_error("Can't find dma allocator (trying to use " + std::string(deviceName) + ")");
  return h;
}
