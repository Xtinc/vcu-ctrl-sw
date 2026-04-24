// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

/****************************************************************************
   -----------------------------------------------------------------------------
 **************************************************************************//*!
   \addtogroup lib_base
   @{
   \file
 *****************************************************************************/
#pragma once

extern "C" {
#include "lib_common/Allocator.h"
#include "lib_common/BufferAPI.h"
#include "lib_common/BufferMeta.h"
}
#include <string>
#include <memory>

/*************************************************************************//*!
   \brief AL_TBufPoolCreateBufCB: Abstraction of buffer creation
*****************************************************************************/
typedef struct AL_TBufPoolCreateBufCB
{
  AL_TBuffer* (*func)(void* pUserParam, AL_TAllocator * pAllocator, PFN_RefCount_CallBack pRefCntCallBack);
  void* userParam;
}AL_TBufPoolCreateBufCB;

/*************************************************************************//*!
   \brief AL_TBufPoolConfig: Used to configure the AL_TBufPool
*****************************************************************************/
typedef struct al_t_BufPoolConfig
{
  AL_TAllocator* pAllocator; /*! allocator used to allocate the buffers */
  uint32_t uNumBuf; /*!< number of buffer in the pool */
  AL_TBufPoolCreateBufCB tCreateBufCB; /*!< abstracted buffer creation function */
}AL_TBufPoolConfig;

/*************************************************************************//*!
   \brief AL_TBufPoolAvailableBufCB: Callback to be notified when a buffer is
   returned to the pool
*****************************************************************************/
typedef struct AL_TBufPoolAvailableBufCB
{
  void (* func)(void* pUserParam);
  void* userParam;
}AL_TBufPoolAvailableBufCB;

/****************************************************************************/
typedef struct
{
  size_t m_zMaxElem;
  size_t m_zTail;
  size_t m_zHead;
  void** m_ElemBuffer;
  AL_MUTEX hMutex;
  AL_EVENT hEvent;
  int m_iBufNumber;
  bool m_isDecommited;
  AL_SEMAPHORE hSpaceSem;
}App_Fifo;

/*************************************************************************//*!
   \brief Buffer Access mode: Do we want to wait if no buffer is available or to fail fast.
*****************************************************************************/
typedef enum AL_EBufMode
{
  AL_BUF_MODE_BLOCK,
  AL_BUF_MODE_NONBLOCK,
  /* sentinel */
  AL_BUF_MODE_MAX
}AL_EBufMode;

uint32_t AL_GetWaitMode(AL_EBufMode eMode);

/*************************************************************************//*!
   \brief AL_TBufPool: Pool of buffer
*****************************************************************************/
typedef struct
{
  AL_TAllocator* pAllocator; /*! Allocator used to allocate the buffers */

  AL_TBuffer** pPool; /*! pool of allocated buffers */
  uint32_t uNumBuf; /*! Number of buffer in the pool */
  AL_TBufPoolAvailableBufCB tAvailableBufCB; /*! Callback to notify availability of a buffer */

  App_Fifo fifo;
}AL_TBufPool;

/*************************************************************************//*!
   \brief AL_BufPool_Init Initialize the AL_TBufPool
   \param[in] pBufPool Pointer to an AL_TBufPool
   \param[in] pConfig Pointer to an AL_TBufPoolConfig object
   \return return true on success, false on failure
*****************************************************************************/
bool AL_BufPool_Init(AL_TBufPool* pBufPool, AL_TBufPoolConfig* pConfig);

/*************************************************************************//*!
   \brief AL_BufPool_Deinit Deiniatilize the AL_TBufPool
   \param[in] pBufPool Pointer to an AL_TBufPool
*****************************************************************************/
void AL_BufPool_Deinit(AL_TBufPool* pBufPool);

/*************************************************************************//*!
   \brief AL_BufPool_RegisterAvailableBufCallback registers a callback to be
   notified when a buffer is returned to the pool, and can be pooled again.
   This method is not thread safe, thus must be called before pool usage.
   \param[in] pBufPool Pointer to an AL_TBufPool
   \param[in] pCB Pointer to the callback
*****************************************************************************/
void AL_BufPool_RegisterAvailableBufCallback(AL_TBufPool* pBufPool, AL_TBufPoolAvailableBufCB* pCB);

/*************************************************************************//*!
   \brief AL_BufPool_GetBuffer Get a buffer from the pool
   \param[in] pBufPool Pointer to an AL_TBufPool
   \param[in] eMode Get mode. blocking or non blocking
   \return return the buffer or NULL in case of failure in the non blocking case
*****************************************************************************/
AL_TBuffer* AL_BufPool_GetBuffer(AL_TBufPool* pBufPool, AL_EBufMode eMode);

/*************************************************************************//*!
   \brief AL_BufPool_AddMetaData creates and adds a metadata on all buffers (even if referenced)
   \param[in] pBufPool Pointer to an AL_TBufPool
   \param[in] pMeta Pointer to a metadata
   \return return true on success, false on failure
*****************************************************************************/
bool AL_BufPool_AddMetaData(AL_TBufPool* pBufPool, AL_TMetaData* pMeta);

/*************************************************************************//*!
   \brief AL_BufPool_Decommit Decommit the pool. This deblocks all the blocking
   call to AL_BufPool_GetBuffer.
   \param[in] pBufPool Pointer to an AL_TBufPool.
*****************************************************************************/
void AL_BufPool_Decommit(AL_TBufPool* pBufPool);
void AL_BufPool_Commit(AL_TBufPool* pBufPool);

/*****************************************************************************/

/*@}*/

#include <stdexcept>
class bufpool_decommited_error : public std::runtime_error
{
public:
  explicit bufpool_decommited_error() : std::runtime_error("bufpool_decommited_error")
  {
  }
};

#include <functional>

/************************    RAII wrapper    *****************************/
struct BaseBufPool
{
  virtual ~BaseBufPool();

  bool Init(AL_TAllocator* pAllocator, uint32_t uNumBuf);
  void RegisterAvailableBufCallback(AL_TBufPoolAvailableBufCB* pCB);
  bool AddMetaData(AL_TMetaData* pMeta);
  AL_TBuffer* GetBuffer(AL_EBufMode mode = AL_BUF_MODE_BLOCK);
  std::shared_ptr<AL_TBuffer> GetSharedBuffer(AL_EBufMode mode = AL_BUF_MODE_BLOCK);
  void Decommit();
  void Commit();

  virtual AL_TBuffer* CreateBuf(AL_TAllocator* pAllocator, PFN_RefCount_CallBack pRefCntCallBack) = 0;

private:
  AL_TBufPool m_pool {};
  static AL_TBuffer* sCreateBuf(void* pUserParam, AL_TAllocator* pAllocator, PFN_RefCount_CallBack pRefCntCallBack);
};

/************************    Default buffer pool    *****************************/
struct BufPool : public BaseBufPool
{
  bool Init(AL_TAllocator* pAllocator, uint32_t uNumBuf, size_t zBufSize, AL_TMetaData* pMeta, std::string const& sName);
  AL_TBuffer* CreateBuf(AL_TAllocator* pAllocator, PFN_RefCount_CallBack pRefCntCallBack) override;
  size_t GetBufSize();
  uint32_t GetNumBuf();

private:
  uint32_t uNumBuf;
  size_t zBufSize;
  std::string sName;
};
