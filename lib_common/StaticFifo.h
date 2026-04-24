// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once
#include "lib_rtos/lib_rtos.h"

/*************************************************************************//*!
  Thread-safe implementation for SINGLE producer/consumer if <int> writes are atomics
  Note that Empty() is head==tail, thus only total_elements-1 entries may be used.
*****************************************************************************/
typedef struct
{
  void** elements;
  int head;
  int tail;
  int total_elements;
}StaticFifo;

bool StaticFifo_Init(StaticFifo* self, void* elements[], int total_elements);

bool StaticFifo_Enqueue(StaticFifo* self, void* element);
void* StaticFifo_Dequeue(StaticFifo* self);
bool StaticFifo_Empty(StaticFifo const* self);

void* StaticFifo_Front(StaticFifo const* self);
int StaticFifo_Size(StaticFifo const* self);
void* StaticFifo_At(StaticFifo const* self, int iOffset);
bool StaticFifo_IsIn(StaticFifo const* self, void* element);
