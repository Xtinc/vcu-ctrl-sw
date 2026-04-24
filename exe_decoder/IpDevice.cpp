// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "IpDevice.h"

#include <stdexcept>
#include <memory>
#include <set>
#include <cassert>
#include <iostream>
#include <string>

#include "IpDevice.h"
#include "IpDeviceCommon.h"
#include "lib_app/console.h"
#include "lib_app/utils.h"

extern "C"
{
#include "lib_common/Allocator.h"
#include "lib_fpga/DmaAlloc.h"
}

using namespace std;

AL_TAllocator* createDmaAllocator(const char* deviceName)
{
  auto h = AL_DmaAlloc_Create(deviceName);

  if(h == nullptr)
    throw runtime_error("Can't find dma allocator (trying to use " + string(deviceName) + ")");
  return h;
}

extern "C"
{
#include "lib_decode/DecSchedulerMcu.h"
}

AL_TAllocator* CreateProxyAllocator(char const*)
{
  // support for the proxy allocator isn't compiled in.
  return nullptr;
}

void CIpDevice::ConfigureMcu(AL_TDriver* driver, bool useProxy)
{
  if(useProxy)
    m_pAllocator = CreateProxyAllocator(this->m_tSelectedDevice.c_str());
  else
    m_pAllocator = createDmaAllocator(this->m_tSelectedDevice.c_str());

  if(!m_pAllocator)
    throw runtime_error("Can't open DMA allocator");

  m_pScheduler = AL_DecSchedulerMcu_Create(driver, this->m_tSelectedDevice.c_str());

  if(!m_pScheduler)
    throw runtime_error("Failed to create MCU scheduler");
}

CIpDevice::~CIpDevice(void)
{
  if(m_pScheduler)
    AL_IDecScheduler_Destroy(m_pScheduler);

  if(m_pAllocator)
    AL_Allocator_Destroy(m_pAllocator);
}

#if defined(__linux__)
#include <dirent.h>
#endif
#include <cstring>

#if defined(__linux__)
static int CountIPDevices(void)
{
  static const char* decDevice = "allegroDecodeIP";

  DIR* devPath = opendir("/dev");

  if(devPath == nullptr)
  {
    throw std::runtime_error("Error: could not open directory");
  }

  struct dirent* entry;
  int iCount = 0;

  while((entry = readdir(devPath)) != nullptr)
  {
    if(std::strncmp(entry->d_name, decDevice, strlen(decDevice) - 1) == 0)
    {
      ++iCount;
    }
  }

  closedir(devPath);

  return iCount;
}
#else
static int CountIPDevices(void)
{
  return 0;
}
#endif

std::string CIpDevice::SelectMcuDevice(std::set<std::string> const& tDevices)
{
  std::string best_device;
  int32_t selected_resources = m_bSelectDeviceWithLowestAvailableResources ? INT32_MAX : -1;

  int nDeviceIndex = 0;

  for(auto const& device : tDevices)
  {
    if(nDeviceIndex >= m_numDevices)
      break;

    if(IsDeviceFailed(device))
    {
      nDeviceIndex++;
      continue;
    }
    AL_IDecScheduler* scheduler = AL_DecSchedulerMcu_Create(AL_GetHardwareDriver(), device.c_str());

    if(scheduler == nullptr)
      throw runtime_error(string("Can't create MCU Scheduler: ") + device);

    int total_resources = 0;
    AL_TIDecSchedulerCore tCore;
    AL_IDecScheduler_Get(scheduler, AL_IDECSCHEDULER_CORE, &tCore);

    for(int iCore = 0; iCore < AL_DEC_NUM_CORES; iCore++)
      total_resources += tCore.iVideoResource[iCore];

    if(!m_bSelectDeviceWithLowestAvailableResources)
    {
      if(total_resources >= selected_resources)
      {
        selected_resources = total_resources;
        best_device = device;
      }
    }
    else
    {
      if(total_resources <= selected_resources)
      {
        selected_resources = total_resources;
        best_device = device;
      }
    }

    nDeviceIndex++;
    AL_IDecScheduler_Destroy(scheduler);
  }

  if(best_device.empty())
    throw runtime_error("Something wrong happened!");

  return best_device;
}

bool CIpDevice::IsDeviceFailed(std::string const& device)
{
  return m_FailedDevices.count(device) > 0;
}

void CIpDevice::SelectNextDevice(void)
{
  m_nDevices++;

  cout << string("Checking with the next available IpDevice") << endl;
  this->m_tSelectedDevice = SelectMcuDevice(m_tDevices);

  if(!this->m_tSelectedDevice.empty())
    ConfigureMcu(AL_GetHardwareDriver(), false);

  m_SelectedDevices[m_nDevices] = this->m_tSelectedDevice;
}

bool CIpDevice::HandleDeviceFailure(void)
{
  bool bCheckNextDevice = false;

  cout << endl << string("Unavailable Resource on: ") << m_SelectedDevices[m_nDevices] << endl;
  m_FailedDevices.insert(m_SelectedDevices[m_nDevices]);

  if(m_nDevices < m_numDevices - 1)
  {
    bCheckNextDevice = true;
  }
  else
  {
    cout << "All devices failed";

    for(int i = 0; i < m_numDevices; i++)
    {
      cout << " - DeviceIP" << i;
    }

    cout << " have unavailable resources" << endl;
    bCheckNextDevice = false;
  }
  return bCheckNextDevice;
}

CIpDevice::CIpDevice(CIpDeviceParam const& param, AL_EDeviceType eDeviceType, std::set<std::string> tDevices) :
  m_tDevices(tDevices)
{
  (void)param;
  this->m_eDeviceType = eDeviceType;

  if(param.iSchedulerType == AL_SCHEDULER_TYPE_MCU)
  {
    m_numDevices = CountIPDevices();
    m_bSelectDeviceWithLowestAvailableResources = param.bSelectDeviceWithLowestAvailableResources;
    this->m_tSelectedDevice = SelectMcuDevice(m_tDevices);
    m_SelectedDevices[m_nDevices] = this->m_tSelectedDevice;
    ConfigureMcu(AL_GetHardwareDriver(), false);
    return;
  }

  throw runtime_error("No support for this scheduling type");
}

AL_EDeviceType CIpDevice::GetDeviceType(void)
{
  return this->m_eDeviceType;
}
