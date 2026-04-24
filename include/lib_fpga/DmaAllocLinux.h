// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/**************************************************************************//*!
   \addtogroup Allocator
   @{
   \file
 *****************************************************************************/
#pragma once

#include "lib_common/Allocator.h"

typedef struct AL_TLinuxDmaAllocator AL_TLinuxDmaAllocator;
/*! \cond ********************************************************************/
typedef struct AL_TDmaAllocLinuxVTable
{
  AL_TAllocatorVTable base;
  int (* pfnGetFd)(AL_TLinuxDmaAllocator* pAllocator, AL_HANDLE hBuf);
  AL_HANDLE (* pfnImportFromFd)(AL_TLinuxDmaAllocator* pAllocator, int fd);
}AL_TDmaAllocLinuxVTable;

struct AL_TLinuxDmaAllocator
{
  AL_TDmaAllocLinuxVTable const* vtable;
};
/*! \endcond *****************************************************************/

/**************************************************************************//*!
   \brief Get the dmabuf file descriptor used by the handle
   The linux dma buffer keeps ownership of the file descriptor. Use dup2 to
   get your own file descriptor if you need to.

   This is useful if you want to import this buffer in your own library / code
   as a dmabuf.

   \param[in] pAllocator a linux dma allocator
   \param[in] hBuf handle of a linux dma buffer
   \return dmabuf file descriptor related to the linux dma buffer.
 *****************************************************************************/
static inline
int AL_LinuxDmaAllocator_GetFd(AL_TLinuxDmaAllocator* pAllocator, AL_HANDLE hBuf)
{
  return pAllocator->vtable->pfnGetFd(pAllocator, hBuf);
}

/**************************************************************************//*!
   \brief Create a buffer handle from a dmabuf file descriptor
   The linux dma buffer doesn't take ownership of the file descriptor.

   This is useful if you want to give a buffer you allocated as a dmabuf yourself
   to the control software.

   \param[in] pAllocator a linux dma allocator
   \param[in] fd a linux dmabuf file descriptor
   \return handle to the dma memory wrapped by the dmabuf file descriptor.
 *****************************************************************************/
static inline
AL_HANDLE AL_LinuxDmaAllocator_ImportFromFd(AL_TLinuxDmaAllocator* pAllocator, int fd)
{
  return pAllocator->vtable->pfnImportFromFd(pAllocator, fd);
}

/*@}*/
