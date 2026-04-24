// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_rtos/lib_rtos.h"

/*************************************************************************//*!
   \brief Version (SemVer) structure
*****************************************************************************/
typedef union AL_TIEncSchedulerVersion
{
  struct
  {
    uint16_t uMajor; /*!< Major version */
    uint16_t uMinor; /*!< Minor version */
    uint32_t uPatch; /*!< Patch version */
  }version;
  AL_64U uVersion; /*!< Version */
}AL_TIEncSchedulerVersion;

/****************************************************************************/
typedef enum AL_EIEncSchedulerInfo
{
  AL_IENCSCHEDULER_VERSION, /*!< reference: AL_TIEncSchedulerVersion */
  AL_IENCSCHEDULER_LOG, /*!< reference: AL_TIEncSchedulerLog */
  AL_IENCSCHEDULER_MAX_ENUM,
}AL_EIEncSchedulerInfo;

static inline char const* ToStringIEncSchedulerInfo(AL_EIEncSchedulerInfo eInfo)
{
  switch(eInfo)
  {
  case AL_IENCSCHEDULER_VERSION: return "AL_IENCSCHEDULER_VERSION";
  case AL_IENCSCHEDULER_LOG: return "AL_IENCSCHEDULER_LOG";
  case AL_IENCSCHEDULER_MAX_ENUM: return "AL_IENCSCHEDULER_MAX_ENUM";

  default: return "Unknown info";
  }

  return "Unknown info";
}
