// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "lib_rtos/lib_rtos.h"

/*************************************************************************//*!
   \brief Core information structure
*****************************************************************************/
typedef struct
{
  int32_t iCoreFrequency;
  int32_t iMaxVideoResourcePerCore;
  int32_t iVideoResource[AL_DEC_NUM_CORES];
}AL_TDecSchedulerCore;

/*************************************************************************//*!
   \brief Version (SemVer) structure
*****************************************************************************/
typedef union
{
  struct
  {
    uint16_t uMajor; /*!< Major version */
    uint16_t uMinor; /*!< Minor version */
    uint32_t uPatch; /*!< Patch version */
  }version;
  AL_64U uVersion; /*!< Version */
}AL_TDecSchedulerVersion;

/*************************************************************************//*!
   \brief Scheduler Log information structure
*****************************************************************************/
typedef struct
{
  bool bEnable;
}AL_TDecSchedulerLog;

/****************************************************************************/
typedef enum
{
  AL_DEC_SCHEDULER_VERSION, /*!< reference: AL_TDecSchedulerVersion */
  AL_DEC_SCHEDULER_CORE, /*!< reference: AL_TDecSchedulerCore */
  AL_DEC_SCHEDULER_SCD_TRACE_CALLBACK, /*!< reference: AL_TDecSchedulerTraceCallback */
  AL_DEC_SCHEDULER_CHANNEL_TRACE_CALLBACK, /*!< reference: AL_TDecSchedulerTraceCallback */
  AL_DEC_SCHEDULER_LOG, /*!< reference: AL_TDecSchedulerLog */
  AL_DEC_SCHEDULER_MAX_ENUM,
}AL_EDecSchedulerInfo;
