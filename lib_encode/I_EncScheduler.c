// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "I_EncScheduler.h"

/* can't be a static inline function as api user need this function and
 * don't know about the AL_IEncScheduler type internals */
void AL_IEncScheduler_Destroy(AL_IEncScheduler* pScheduler)
{
  pScheduler->vtable->destroy(pScheduler);
}

void AL_IEncScheduler_Get(AL_IEncScheduler const* pScheduler, AL_EIEncSchedulerInfo eInfo, void* pParam)
{
  pScheduler->vtable->get(pScheduler, eInfo, pParam);
}

void AL_IEncScheduler_Set(AL_IEncScheduler* pScheduler, AL_EIEncSchedulerInfo eInfo, void const* pParam)
{
  pScheduler->vtable->set(pScheduler, eInfo, pParam);
}
