// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include <stdexcept>
#include "lib_app/BufPool.h"

extern "C"
{
#include "lib_rtos/lib_rtos.h"
#include "lib_common/Allocator.h"
}

static bool Fifo_Init(App_Fifo* pFifo, size_t zMaxElem);
static void Fifo_Deinit(App_Fifo* pFifo);
static bool Fifo_Queue(App_Fifo* pFifo, void* pElem, uint32_t uWait);
static void* Fifo_Dequeue(App_Fifo* pFifo, uint32_t uWait);
static void Fifo_Decommit(App_Fifo* pFifo);
static void Fifo_Commit(App_Fifo* pFifo);
static size_t Fifo_GetMaxElements(App_Fifo* pFifo);

/****************************************************************************/
static void AL_sBufPool_QueueBuf(AL_TBufPool* pBufPool, AL_TBuffer* pBuf)
{
  Fifo_Queue(&pBufPool->fifo, pBuf, AL_WAIT_FOREVER);
}

static void AL_sBufPool_FreeBufInPool(AL_TBuffer* pBuf)
{
  auto pBufPool = (AL_TBufPool*)AL_Buffer_GetUserData(pBuf);
  bool bBufferQueued = Fifo_Queue(&pBufPool->fifo, pBuf, AL_WAIT_FOREVER);

  if(bBufferQueued && pBufPool->tAvailableBufCB.func != NULL)
    pBufPool->tAvailableBufCB.func(pBufPool->tAvailableBufCB.userParam);
}

static AL_TBuffer* AL_sBufPool_CreateBuffer(AL_TBufPoolConfig& config, AL_TAllocator* pAllocator)
{
  return config.tCreateBufCB.func(config.tCreateBufCB.userParam, pAllocator, AL_sBufPool_FreeBufInPool);
}

/****************************************************************************/
static bool AL_sBufPool_AddBuf(AL_TBufPool* pBufPool, AL_TBuffer* pBuf)
{
  if(!pBuf)
    return false;

  if(pBufPool->uNumBuf >= Fifo_GetMaxElements(&pBufPool->fifo))
    return false;

  AL_Buffer_SetUserData(pBuf, pBufPool);
  pBufPool->pPool[pBufPool->uNumBuf++] = pBuf;
  AL_sBufPool_QueueBuf(pBufPool, pBuf);
  return true;
}

/****************************************************************************/
static bool AL_sBufPool_AddAllocBuf(AL_TBufPool* pBufPool, AL_TBufPoolConfig* pConfig)
{
  AL_TBuffer* pBuf = AL_sBufPool_CreateBuffer(*pConfig, pBufPool->pAllocator);
  return AL_sBufPool_AddBuf(pBufPool, pBuf);
}

/****************************************************************************/
static bool AL_sBufPool_InitStructure(AL_TBufPool* pBufPool, AL_TBufPoolConfig* pConfig)
{
  size_t zMemPoolSize = 0;

  if(!pBufPool)
    return false;

  if(!pConfig->pAllocator)
    return false;

  pBufPool->pAllocator = pConfig->pAllocator;

  if(!Fifo_Init(&pBufPool->fifo, pConfig->uNumBuf))
    return false;

  pBufPool->uNumBuf = 0;

  zMemPoolSize = pConfig->uNumBuf * sizeof(AL_TBuffer*);

  pBufPool->pPool = (AL_TBuffer**)Rtos_Malloc(zMemPoolSize);

  if(!pBufPool->pPool)
  {
    AL_BufPool_Deinit(pBufPool);
    return false;
  }

  pBufPool->tAvailableBufCB.func = NULL;
  pBufPool->tAvailableBufCB.userParam = NULL;

  return true;
}

/****************************************************************************/
bool AL_BufPool_Init(AL_TBufPool* pBufPool, AL_TBufPoolConfig* pConfig)
{
  if(!AL_sBufPool_InitStructure(pBufPool, pConfig))
    return false;

  // Create uMin free buffers
  while(pBufPool->uNumBuf < pConfig->uNumBuf)
  {
    if(!AL_sBufPool_AddAllocBuf(pBufPool, pConfig))
    {
      AL_BufPool_Deinit(pBufPool);
      return false;
    }
  }

  return true;
}

/****************************************************************************/
void AL_BufPool_Deinit(AL_TBufPool* pBufPool)
{
  for(uint32_t u = 0; u < pBufPool->uNumBuf; ++u)
  {
    AL_TBuffer* pBuf = pBufPool->pPool[u];
    AL_Buffer_Destroy(pBuf);
    pBufPool->pPool[u] = NULL;
  }

  Fifo_Deinit(&pBufPool->fifo);
  Rtos_Free(pBufPool->pPool);
  Rtos_Memset(pBufPool, 0, sizeof(*pBufPool));
}

/****************************************************************************/
void AL_BufPool_RegisterAvailableBufCallback(AL_TBufPool* pBufPool, AL_TBufPoolAvailableBufCB* pCB)
{
  pBufPool->tAvailableBufCB = *pCB;
}

/****************************************************************************/
AL_TBuffer* AL_BufPool_GetBuffer(AL_TBufPool* pBufPool, AL_EBufMode eMode)
{
  uint32_t Wait = AL_GetWaitMode(eMode);

  auto pBuf = (AL_TBuffer*)Fifo_Dequeue(&pBufPool->fifo, Wait);

  if(!pBuf)
    return NULL;

  AL_Buffer_Ref(pBuf);
  return pBuf;
}

/****************************************************************************/
bool AL_BufPool_AddMetaData(AL_TBufPool* pBufPool, AL_TMetaData* pMetaData)
{
  AL_TMetaData* pMeta;
  AL_TBuffer* pBuf;

  for(uint32_t u = 0; u < pBufPool->uNumBuf; ++u)
  {
    pBuf = pBufPool->pPool[u];
    pMeta = AL_MetaData_Clone(pMetaData);

    if(!AL_Buffer_AddMetaData(pBuf, pMeta))
      return false;
  }

  return true;
}

/****************************************************************************/
void AL_BufPool_Decommit(AL_TBufPool* pBufPool)
{
  Fifo_Decommit(&pBufPool->fifo);
}

/****************************************************************************/
void AL_BufPool_Commit(AL_TBufPool* pBufPool)
{
  Fifo_Commit(&pBufPool->fifo);
}

/****************************************************************************/
static bool Fifo_Init(App_Fifo* pFifo, size_t zMaxElem)
{
  pFifo->m_zMaxElem = zMaxElem + 1;
  pFifo->m_zTail = 0;
  pFifo->m_zHead = 0;
  pFifo->m_iBufNumber = 0;
  pFifo->m_isDecommited = false;

  size_t zElemSize = pFifo->m_zMaxElem * sizeof(void*);
  pFifo->m_ElemBuffer = (void**)Rtos_Malloc(zElemSize);

  if(!pFifo->m_ElemBuffer)
    return false;
  Rtos_Memset(pFifo->m_ElemBuffer, 0xCD, zElemSize);

  pFifo->hEvent = Rtos_CreateEvent(0);

  if(!pFifo->hEvent)
  {
    Rtos_Free(pFifo->m_ElemBuffer);
    return false;
  }

  pFifo->hSpaceSem = Rtos_CreateSemaphore(zMaxElem);
  pFifo->hMutex = Rtos_CreateMutex();

  if(!pFifo->hSpaceSem)
  {
    Rtos_DeleteEvent(pFifo->hEvent);
    Rtos_Free(pFifo->m_ElemBuffer);
    return false;
  }

  return true;
}

static void Fifo_Deinit(App_Fifo* pFifo)
{
  Rtos_Free(pFifo->m_ElemBuffer);
  Rtos_DeleteEvent(pFifo->hEvent);
  Rtos_DeleteSemaphore(pFifo->hSpaceSem);
  Rtos_DeleteMutex(pFifo->hMutex);
}

static bool Fifo_Queue(App_Fifo* pFifo, void* pElem, uint32_t uWait)
{
  if(!Rtos_GetSemaphore(pFifo->hSpaceSem, uWait))
    return false;

  Rtos_GetMutex(pFifo->hMutex);
  pFifo->m_ElemBuffer[pFifo->m_zTail] = pElem;
  pFifo->m_zTail = (pFifo->m_zTail + 1) % pFifo->m_zMaxElem;
  ++pFifo->m_iBufNumber;
  Rtos_SetEvent(pFifo->hEvent);
  Rtos_ReleaseMutex(pFifo->hMutex);

  /* new item was added in the queue */
  return true;
}

static void* Fifo_Dequeue(App_Fifo* pFifo, uint32_t uWait)
{
  /* wait if no items */
  Rtos_GetMutex(pFifo->hMutex);
  bool failed = false;

  while(true)
  {
    if(pFifo->m_iBufNumber > 0)
      break;

    if(failed || pFifo->m_isDecommited)
    {
      Rtos_ReleaseMutex(pFifo->hMutex);
      return NULL;
    }

    Rtos_ReleaseMutex(pFifo->hMutex);

    if(!Rtos_WaitEvent(pFifo->hEvent, uWait))
      failed = true;

    Rtos_GetMutex(pFifo->hMutex);
  }

  void* pElem = pFifo->m_ElemBuffer[pFifo->m_zHead];
  pFifo->m_zHead = (pFifo->m_zHead + 1) % pFifo->m_zMaxElem;
  --pFifo->m_iBufNumber;
  Rtos_ReleaseMutex(pFifo->hMutex);

  /* new empty space available */
  Rtos_ReleaseSemaphore(pFifo->hSpaceSem);
  return pElem;
}

static void Fifo_Decommit(App_Fifo* pFifo)
{
  Rtos_GetMutex(pFifo->hMutex);
  pFifo->m_isDecommited = true;
  Rtos_SetEvent(pFifo->hEvent);
  Rtos_ReleaseMutex(pFifo->hMutex);
}

/* Protected by mutex, but to be really useful, you need to know that you already
 * used the decommit feature successfully and you want to reuse the bufpool again
 * after that. */
static void Fifo_Commit(App_Fifo* pFifo)
{
  Rtos_GetMutex(pFifo->hMutex);
  pFifo->m_isDecommited = false;
  Rtos_ReleaseMutex(pFifo->hMutex);
}

static size_t Fifo_GetMaxElements(App_Fifo* pFifo)
{
  size_t sMaxElem = 0;
  Rtos_GetMutex(pFifo->hMutex);
  sMaxElem = pFifo->m_zMaxElem - 1;
  Rtos_ReleaseMutex(pFifo->hMutex);
  return sMaxElem;
}

uint32_t AL_GetWaitMode(AL_EBufMode eMode)
{
  uint32_t Wait = 0;
  switch(eMode)
  {
  case AL_BUF_MODE_BLOCK:
    Wait = AL_WAIT_FOREVER;
    break;
  case AL_BUF_MODE_NONBLOCK:
    Wait = AL_NO_WAIT;
    break;
  default:

    if(eMode < AL_BUF_MODE_MAX)
      throw std::runtime_error("eMode should be higher or equal than AL_BUF_MODE_MAX");
    break;
  }

  return Wait;
}

BaseBufPool::~BaseBufPool(void)
{
  AL_BufPool_Deinit(&m_pool);
}

bool BaseBufPool::Init(AL_TAllocator* pAllocator, uint32_t uNumBuf)
{
  AL_TBufPoolCreateBufCB tCreateBufCB =
  {
    sCreateBuf,
    this
  };

  AL_TBufPoolConfig tConfig =
  {
    pAllocator,
    uNumBuf,
    tCreateBufCB
  };

  return AL_BufPool_Init(&m_pool, &tConfig);
}

void BaseBufPool::RegisterAvailableBufCallback(AL_TBufPoolAvailableBufCB* pCB)
{
  AL_BufPool_RegisterAvailableBufCallback(&m_pool, pCB);
}

bool BaseBufPool::AddMetaData(AL_TMetaData* pMeta)
{
  return AL_BufPool_AddMetaData(&m_pool, pMeta);
}

AL_TBuffer* BaseBufPool::GetBuffer(AL_EBufMode mode)
{
  AL_TBuffer* pBuf = AL_BufPool_GetBuffer(&m_pool, mode);

  if(mode == AL_BUF_MODE_BLOCK && pBuf == nullptr)
    throw bufpool_decommited_error();

  return pBuf;
}

std::shared_ptr<AL_TBuffer> BaseBufPool::GetSharedBuffer(AL_EBufMode mode)
{
  AL_TBuffer* pBuf = GetBuffer(mode);

  if(pBuf == nullptr)
    return nullptr;

  return std::shared_ptr<AL_TBuffer>(pBuf, &AL_Buffer_Unref);
}

void BaseBufPool::Decommit(void)
{
  AL_BufPool_Decommit(&m_pool);
}

void BaseBufPool::Commit(void)
{
  AL_BufPool_Commit(&m_pool);
}

AL_TBuffer* BaseBufPool::sCreateBuf(void* pUserParam, AL_TAllocator* pAllocator, PFN_RefCount_CallBack pRefCntCallBack)
{
  BaseBufPool* pBufPool = (BaseBufPool*)pUserParam;
  return pBufPool->CreateBuf(pAllocator, pRefCntCallBack);
}

bool BufPool::Init(AL_TAllocator* pAllocator, uint32_t uNumBuf, size_t zBufSize, AL_TMetaData* pMeta, std::string const& sName)
{
  this->uNumBuf = uNumBuf;
  this->zBufSize = zBufSize;
  this->sName = sName;

  if(!BaseBufPool::Init(pAllocator, uNumBuf))
    return false;

  return pMeta == nullptr || AddMetaData(pMeta);
}

AL_TBuffer* BufPool::CreateBuf(AL_TAllocator* pAllocator, PFN_RefCount_CallBack pRefCntCallBack)
{
  return AL_Buffer_Create_And_AllocateNamed(pAllocator, zBufSize, pRefCntCallBack, sName.c_str());
}

size_t BufPool::GetBufSize(void)
{
  return zBufSize;
}

uint32_t BufPool::GetNumBuf(void)
{
  return uNumBuf;
}
