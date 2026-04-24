// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once
#include <functional>
#include <set>
#include <string>
#include <array>
#include "lib_app/utils.h"
#include "IpDeviceCommon.h"

extern "C"
{
#include "lib_decode/lib_decode.h"
#include "lib_common_dec/DecChanParam.h"
}

typedef struct AL_IDecScheduler AL_IDecScheduler;

/*****************************************************************************/
class CIpDevice : public I_IpDevice
{
public:
  CIpDevice(CIpDeviceParam const& param, AL_EDeviceType eDeviceType, std::set<std::string> tDevices);
  ~CIpDevice();

  AL_EDeviceType GetDeviceType();
  void* GetScheduler() override;
  AL_TAllocator* GetAllocator() override;

  CIpDevice(CIpDevice const &) = delete;
  CIpDevice & operator = (CIpDevice const &) = delete;

  void SelectNextDevice();
  bool HandleDeviceFailure();
  bool IsDeviceFailed(std::string const& device);

private:
  std::set<std::string> const m_tDevices;
  std::string m_tSelectedDevice;
  AL_EDeviceType m_eDeviceType;
  AL_IDecScheduler* m_pScheduler = nullptr;
  AL_TAllocator* m_pAllocator = nullptr;
  std::set<std::string> m_FailedDevices;
  int m_nDevices = 0;
  std::array<std::string, 4> m_SelectedDevices;
  bool m_bSelectDeviceWithLowestAvailableResources;
  int m_numDevices;

  void ConfigureMcu(AL_TDriver* driver, bool useProxy);
  std::string SelectMcuDevice(std::set<std::string> const& tDevices);
};

inline void* CIpDevice::GetScheduler(void)
{
  return m_pScheduler;
}

inline AL_TAllocator* CIpDevice::GetAllocator(void)
{
  return m_pAllocator;
}
