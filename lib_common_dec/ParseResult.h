// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

typedef enum
{
  AL_OK,
  AL_CONCEAL,
  AL_BAD_ID,
  AL_UNSUPPORTED,
  AL_LAUNCHED_OK,
}AL_PARSE_RESULT;

#include "lib_rtos/lib_rtos.h"

#define COMPLY(cond) \
  do { \
    if(!(cond)) \
      return AL_CONCEAL; \
  } \
  while(0)

#define COMPLY_ID(cond) \
  do { \
    if(!(cond)) \
      return AL_BAD_ID; \
  } \
  while(0)

#define COMPLY_WITH_LOG(cond, log) \
  do { \
    if(!(cond)) \
    { \
      VIDEO_ERROR_PRINT("%s", log); \
      return AL_CONCEAL; \
    } \
  } \
  while(0)
