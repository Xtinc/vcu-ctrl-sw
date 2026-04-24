// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "StaticFifo.h"
#include "lib_rtos/message.h"

bool StaticFifo_Init(StaticFifo* self, void* elements[], int total_elements)
{
  if(NULL == self)
    return false;

  if(NULL == elements)
    return false;

  if(total_elements < 1)
    return false;
  self->head = 0;
  self->tail = 0;
  self->total_elements = total_elements;
  self->elements = elements;
  return true;
}

bool StaticFifo_Empty(StaticFifo const* self)
{
  return self->head == self->tail;
}

int StaticFifo_Size(StaticFifo const* self)
{
  return (self->tail + self->total_elements - self->head) % self->total_elements;
}

bool StaticFifo_Enqueue(StaticFifo* self, void* element)
{
  if(((self->tail + 1) % self->total_elements) == self->head)
    return false;

  self->elements[self->tail] = element;
  self->tail = (self->tail + 1) % self->total_elements;
  return true;
}

void* StaticFifo_Dequeue(StaticFifo* self)
{
  if(StaticFifo_Empty(self))
    return NULL;

  void* element = self->elements[self->head];
  self->head = (self->head + 1) % self->total_elements;
  return element;
}

void* StaticFifo_Front(StaticFifo const* self)
{
  return StaticFifo_At(self, 0);
}

void* StaticFifo_At(StaticFifo const* self, int iOffset)
{
  if(StaticFifo_Empty(self))
    return NULL;

  return self->elements[(self->head + iOffset) % self->total_elements];
}

bool StaticFifo_IsIn(StaticFifo const* self, void* element)
{
  if(StaticFifo_Empty(self))
    return false;

  for(int index = self->head; index != self->tail; index = ((index + 1) % self->total_elements))
  {
    if(element == self->elements[index])
      return true;
  }

  return false;
}
