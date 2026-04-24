// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "lib_encode/I_EncScheduler.h"
#include "lib_common/IDriver.h"
#include "lib_common/Allocator.h"

#if __linux__

#include "lib_encode/I_EncSchedulerInfo.h"
#include "lib_encode/EncSchedulerMcu.h"
#include "lib_encode/EncSchedulerCommon.h"
#include "lib_common/Error.h"
#include "lib_common_enc/RateCtrlMeta.h"
#include "lib_rtos/lib_rtos.h"
#include "allegro_ioctl_mcu_enc.h"
#include "DriverDataConversions.h"
#include "lib_fpga/DmaAllocLinux.h"
#include "lib_common/Utils.h"
#include "lib_rtos/message.h"

#include <stdio.h>
#include <string.h> // strerrno, strlen, strcpy
#include <errno.h>
#include <unistd.h> // for close

typedef struct
{
  const AL_IEncSchedulerVtable* vtable;
  AL_TLinuxDmaAllocator* allocator;
  AL_TDriver* driver;
  char* deviceFile;
}AL_TEncSchedulerMicroblaze;

typedef struct
{
  AL_TEncScheduler_CB_EndEncoding CBs;
  AL_TCommonChannelInfo info;
  AL_TDriver* driver;
  int fd;
  AL_THREAD thread;
  bool outputRec;
}AL_TEncChannelMicroblaze;

static void* WaitForStatus(void* p);

static void setCallbacks(AL_TEncChannelMicroblaze* pChannel, AL_TEncScheduler_CB_EndEncoding* pCBs)
{
  pChannel->CBs.func = pCBs->func;
  pChannel->CBs.userParam = pCBs->userParam;
}

static AL_ERR API_CreateChannel(AL_HANDLE* hChannel, AL_IEncScheduler* pIScheduler, TMemDesc* pMDChParam, TMemDesc* pEP1, AL_HANDLE hRcPluginDmaContext, AL_TEncScheduler_CB_EndEncoding* pCBs)
{
  (void)hRcPluginDmaContext;
  AL_ERR errorCode = AL_ERROR;
  AL_TEncSchedulerMicroblaze* pScheduler = (AL_TEncSchedulerMicroblaze*)pIScheduler;
  AL_TEncChannelMicroblaze* pChannel = Rtos_Malloc(sizeof(*pChannel));

  if(NULL == pChannel)
  {
    errorCode = AL_ERR_NO_MEMORY;
    goto channel_creation_fail;
  }

  Rtos_Memset(pChannel, 0, sizeof(*pChannel));

  pChannel->driver = pScheduler->driver;
  pChannel->fd = AL_Driver_Open(pChannel->driver, pScheduler->deviceFile);

  if(pChannel->fd < 0)
  {
    fprintf(stderr, "Couldn't open device file %s while creating channel; %s\n", pScheduler->deviceFile, strerror(errno));
    goto driver_open_fail;
  }

  AL_TEncChanParam* pChParam = ((AL_TEncChanParam*)pMDChParam->pVirtualAddr);
  struct al5_channel_config msg = { 0 };
  setChannelParam(&msg.param, pMDChParam, pEP1);

  msg.rc_plugin_fd = -1;

  if(hRcPluginDmaContext)
    msg.rc_plugin_fd = AL_LinuxDmaAllocator_GetFd(pScheduler->allocator, hRcPluginDmaContext);

  pChannel->outputRec = pChParam->eEncOptions & AL_OPT_FORCE_REC;

  AL_EDriverError errdrv = AL_Driver_PostBlockingMessage(pChannel->driver, pChannel->fd, AL_MCU_CONFIG_CHANNEL, &msg);

  if(errdrv != DRIVER_SUCCESS)
  {
    if(errdrv == DRIVER_ERROR_NO_MEMORY)
      errorCode = AL_ERR_NO_MEMORY;

    /* the ioctl might not have been called at all,
     * so the error_code might no be set. leave it to AL_ERROR in this case */
    if((errdrv == DRIVER_ERROR_CHANNEL) && (msg.status.error_code != 0))
      errorCode = msg.status.error_code;

    goto fail;
  }

  DBG_ASSERT_COND(!AL_IS_ERROR_CODE(msg.status.error_code));

  setCallbacks(pChannel, pCBs);
  pChannel->thread = Rtos_CreateThread(&WaitForStatus, pChannel);

  if(!pChannel->thread)
    goto fail;

  /* We assume we configure the rec buffers the same way on MCU side */
  SetChannelInfo(&pChannel->info, (AL_TEncChanParam*)pMDChParam->pVirtualAddr);

  *hChannel = (AL_HANDLE)pChannel;
  return AL_SUCCESS;

  fail:
  AL_Driver_Close(pScheduler->driver, pChannel->fd);
  driver_open_fail:
  Rtos_Free(pChannel);
  channel_creation_fail:
  *hChannel = AL_INVALID_CHANNEL;
  return errorCode;
}

static void createEncodeMsg(struct al5_encode_msg* msg, AL_TEncInfo* pEncInfo, AL_TEncRequestInfo* pReqInfo, AL_TEncPicBufAddrs* pBuffersAddrs)
{
  if(!pEncInfo || !pReqInfo || !pBuffersAddrs)
  {
    msg->params.size = 0;
    msg->addresses.size = 0;
    return;
  }

  if(pBuffersAddrs->pEP2)
    pBuffersAddrs->pEP2_v = (pBuffersAddrs->pEP2 & 0x7FFFFFFF) + DCACHE_OFFSET;
  else
    pBuffersAddrs->pEP2_v = 0;
  setEncodeMsg(msg, pEncInfo, pReqInfo, pBuffersAddrs);
}

static bool API_EncodeOneFrame(AL_IEncScheduler* pIScheduler, AL_HANDLE hChannel, AL_TEncInfo* pEncInfo, AL_TEncRequestInfo* pReqInfo, AL_TEncPicBufAddrs* pBuffersAddrs)
{
  AL_TEncSchedulerMicroblaze* pScheduler = (AL_TEncSchedulerMicroblaze*)pIScheduler;
  AL_TEncChannelMicroblaze* pChannel = (AL_TEncChannelMicroblaze*)hChannel;
  struct al5_encode_msg msg = { 0 };
  createEncodeMsg(&msg, pEncInfo, pReqInfo, pBuffersAddrs);
  return AL_Driver_PostBlockingMessage(pScheduler->driver, pChannel->fd, AL_MCU_ENCODE_ONE_FRM, &msg) == DRIVER_SUCCESS;
}

static bool API_DestroyChannel(AL_IEncScheduler* pIScheduler, AL_HANDLE hChannel)
{
  AL_TEncSchedulerMicroblaze* pScheduler = (AL_TEncSchedulerMicroblaze*)pIScheduler;
  AL_TEncChannelMicroblaze* pChannel = (AL_TEncChannelMicroblaze*)hChannel;

  if(NULL == pChannel)
    return false;

  AL_Driver_PostBlockingMessage(pScheduler->driver, pChannel->fd, AL_MCU_DESTROY_CHANNEL, NULL);

  if(!Rtos_JoinThread(pChannel->thread))
    return false;
  Rtos_DeleteThread(pChannel->thread);

  AL_Driver_Close(pScheduler->driver, pChannel->fd);

  Rtos_Free(pChannel);

  return true;
}

static bool API_GetRecPicture(AL_IEncScheduler* pIScheduler, AL_HANDLE hChannel, AL_TRecPic* pRecPic)
{
  AL_TEncSchedulerMicroblaze* pScheduler = (AL_TEncSchedulerMicroblaze*)pIScheduler;
  AL_TEncChannelMicroblaze* pChannel = (AL_TEncChannelMicroblaze*)hChannel;
  struct al5_reconstructed_info msg = { 0 };

  if(!pChannel->outputRec)
    return false;

  if(AL_Driver_PostBlockingMessage(pScheduler->driver, pChannel->fd, AL_MCU_GET_REC_PICTURE, &msg) != DRIVER_SUCCESS)
    return false;

  AL_TLinuxDmaAllocator* pAllocator = pScheduler->allocator;
  AL_HANDLE hRecBuf = AL_LinuxDmaAllocator_ImportFromFd(pAllocator, msg.fd);

  if(!hRecBuf)
    return false;

  AL_TReconstructedInfo recInfo;
  recInfo.uID = AL_LinuxDmaAllocator_GetFd((AL_TLinuxDmaAllocator*)pAllocator, hRecBuf);
  recInfo.ePicStruct = msg.pic_struct;
  recInfo.iPOC = msg.poc;

  recInfo.tPicDim.iWidth = msg.width;
  recInfo.tPicDim.iHeight = msg.height;

  SetRecPic(pRecPic, (AL_TAllocator*)pAllocator, hRecBuf, &pChannel->info, &recInfo);

  return true;
}

static bool API_ReleaseRecPicture(AL_IEncScheduler* pIScheduler, AL_HANDLE hChannel, AL_TRecPic* pRecPic)
{
  AL_TEncSchedulerMicroblaze* pScheduler = (AL_TEncSchedulerMicroblaze*)pIScheduler;
  AL_TEncChannelMicroblaze* pChannel = hChannel;

  if(!pRecPic->pBuf || !pChannel->outputRec)
    return false;

  AL_HANDLE hRecBuf = pRecPic->pBuf->hBufs[0];
  AL_TLinuxDmaAllocator* pAllocator = pScheduler->allocator;
  __u32 fd = AL_LinuxDmaAllocator_GetFd(pAllocator, hRecBuf);

  if(AL_Driver_PostBlockingMessage(pScheduler->driver, pChannel->fd, AL_MCU_RELEASE_REC_PICTURE, &fd) != DRIVER_SUCCESS)
    return false;

  AL_Allocator_Free((AL_TAllocator*)pAllocator, hRecBuf);
  close(fd);

  return true;
}

static void processStatusMsg(AL_TEncChannelMicroblaze* pChannel, struct al5_params* msg)
{
  DBG_ASSERT_COND(msg->size >= sizeof(AL_PTR64));
  AL_PTR64 streamBufferPtr;
  Rtos_Memcpy(&streamBufferPtr, msg->opaque, sizeof(AL_PTR64));
  AL_TEncPicStatus* pStatus = NULL;
  AL_TEncPicStatus status;

  if(msg->size > sizeof(AL_PTR64))
  {
    DBG_ASSERT_COND(msg->size == sizeof(AL_PTR64) + sizeof(AL_TEncPicStatus));
    Rtos_Memcpy(&status, (char*)msg->opaque + sizeof(AL_PTR64), UnsignedMin(sizeof(status), sizeof(msg->opaque) - sizeof(AL_PTR64)));
    pStatus = &status;
  }

  pChannel->CBs.func(pChannel->CBs.userParam, pStatus, streamBufferPtr);
}

static void* WaitForStatus(void* p)
{
  Rtos_SetCurrentThreadName("enc-status-it");
  AL_TEncChannelMicroblaze* pChannel = (AL_TEncChannelMicroblaze*)p;
  struct al5_params msg = { 0 };
  Rtos_PollCtx ctx;
  /* Wait for wait for status events forever in poll */
  ctx.timeout = -1;
  ctx.events = AL_POLLIN;

  while(true)
  {
    ctx.revents = 0;

    AL_EDriverError err = AL_Driver_PostBlockingMessage(pChannel->driver, pChannel->fd, AL_POLL_MSG, &ctx);

    if(err != DRIVER_SUCCESS)
      continue;

    if(ctx.revents & AL_POLLIN)
    {
      AL_EDriverError err = AL_Driver_PostNonBlockingMessage(pChannel->driver, pChannel->fd, AL_MCU_WAIT_FOR_STATUS, &msg);

      if(err == DRIVER_SUCCESS)
        processStatusMsg(pChannel, &msg);
      else
        Rtos_Log(AL_LOG_ERROR, "Failed to get encode status (error code: %d)\n", err);
    }

    /* If the polling finds an end of operation, it means that the channel was destroyed and we can stop waiting for encoding results. */
    if(ctx.revents & AL_POLLHUP)
    {
      break;
    }
  }

  return NULL;
}

static void API_Destroy(AL_IEncScheduler* pIScheduler)
{
  AL_TEncSchedulerMicroblaze* pScheduler = (AL_TEncSchedulerMicroblaze*)pIScheduler;
  Rtos_Free(pScheduler->deviceFile);
  Rtos_Free(pScheduler);
}

static __u32 getFd(AL_TBuffer const* pBuffer)
{
  return (__u32)AL_LinuxDmaAllocator_GetFd((AL_TLinuxDmaAllocator*)pBuffer->pAllocator, pBuffer->hBufs[0]);
}

static void createPutStreamMsg(struct al5_buffer* msg, AL_TBuffer* streamBuffer, AL_64U streamUserPtr, uint32_t uOffset)
{
  Rtos_Memset(msg, 0, sizeof(*msg));
  msg->stream_buffer.handle = getFd(streamBuffer);
  msg->stream_buffer.offset = uOffset;
  msg->stream_buffer.stream_buffer_ptr = streamUserPtr;
  msg->stream_buffer.size = AL_Buffer_GetSize(streamBuffer);
  msg->external_mv_handle = 0;

  AL_TRateCtrlMetaData* pMeta = (AL_TRateCtrlMetaData*)AL_Buffer_GetMetaData(streamBuffer, AL_META_TYPE_RATECTRL);

  if(pMeta != NULL)
    msg->external_mv_handle = getFd(pMeta->pMVBuf);
}

static void API_PutStreamBuffer(AL_IEncScheduler* pIScheduler, AL_HANDLE hChannel, AL_TBuffer* streamBuffer, AL_64U streamUserPtr, uint32_t uOffset)
{
  DBG_ASSERT_COND(streamBuffer);
  AL_TEncSchedulerMicroblaze* pScheduler = (AL_TEncSchedulerMicroblaze*)pIScheduler;
  AL_TEncChannelMicroblaze* pChannel = (AL_TEncChannelMicroblaze*)hChannel;
  struct al5_buffer driverBuffer;
  createPutStreamMsg(&driverBuffer, streamBuffer, streamUserPtr, uOffset);
  AL_Driver_PostBlockingMessage(pScheduler->driver, pChannel->fd, AL_MCU_PUT_STREAM_BUFFER, &driverBuffer);
}

/******************************************************************************/
static void GetSchedulerVersion(AL_TEncSchedulerMicroblaze const* pScheduler, AL_TIEncSchedulerVersion* pVersion)
{
  int const fd = AL_Driver_Open(pScheduler->driver, pScheduler->deviceFile);

  if(fd < 0)
  {
    Rtos_Log(AL_LOG_ERROR, "Couldn't open device file '%s' while creating channel: '%s'\n", pScheduler->deviceFile, strerror(errno));
    return;
  }

  struct al5_params msg;
  AL_EIEncSchedulerInfo eInfo = AL_IENCSCHEDULER_VERSION;
  msg.opaque[0] = eInfo;
  memcpy(&msg.opaque[sizeof(eInfo) / sizeof(*msg.opaque)], pVersion, sizeof(*pVersion));

  static_assert(sizeof(eInfo) + sizeof(*pVersion) <= sizeof(msg.opaque), "Driver version structure struct is too small");
  msg.size = sizeof(eInfo) + sizeof(*pVersion);

  AL_EDriverError const error = AL_Driver_PostBlockingMessage(pScheduler->driver, fd, AL_MCU_GET, &msg);

  if(error != DRIVER_SUCCESS)
  {
    Rtos_Log(AL_LOG_ERROR, "Failed to get parameter '%s', (error code: '%d')\n", ToStringIEncSchedulerInfo(AL_IENCSCHEDULER_VERSION), error);
    AL_Driver_Close(pScheduler->driver, fd);
    return;
  }

  memcpy(pVersion, &msg.opaque[1], sizeof(*pVersion));

  AL_Driver_Close(pScheduler->driver, fd);
}

static void API_Get(AL_IEncScheduler const* pScheduler, AL_EIEncSchedulerInfo info, void* pParam)
{
  AL_TEncSchedulerMicroblaze const* pSchedulerMcu = (AL_TEncSchedulerMicroblaze const*)pScheduler;
  switch(info)
  {
  case AL_IENCSCHEDULER_VERSION:
  {
    GetSchedulerVersion(pSchedulerMcu, (AL_TIEncSchedulerVersion*)pParam);
    return;
  }
  default: return;
  }

  return;
}

static void API_Set(AL_IEncScheduler* pScheduler, AL_EIEncSchedulerInfo info, void const* pParam)
{
  (void)pParam;
  AL_TEncSchedulerMicroblaze* pSchedulerMcu = (AL_TEncSchedulerMicroblaze*)pScheduler;
  (void)pSchedulerMcu;
  switch(info)
  {
  default: return;
  }

  return;
}

static const AL_IEncSchedulerVtable McuEncSchedulerVtable =
{
  API_Destroy,
  API_CreateChannel,
  API_DestroyChannel,
  API_EncodeOneFrame,
  API_PutStreamBuffer,
  API_GetRecPicture,
  API_ReleaseRecPicture,
  API_Get,
  API_Set,
};

AL_IEncScheduler* AL_SchedulerMcu_Create(AL_TDriver* driver, AL_TLinuxDmaAllocator* pDmaAllocator, char const* deviceFile)
{
  AL_TEncSchedulerMicroblaze* pScheduler = Rtos_Malloc(sizeof(*pScheduler));

  if(NULL == pScheduler)
    return NULL;

  pScheduler->vtable = &McuEncSchedulerVtable;
  pScheduler->driver = driver;
  pScheduler->allocator = pDmaAllocator;
  pScheduler->deviceFile = Rtos_Malloc((strlen(deviceFile) + 1) * sizeof(char));

  if(NULL == pScheduler->deviceFile)
  {
    Rtos_Free(pScheduler);
    return NULL;
  }

  strcpy(pScheduler->deviceFile, deviceFile);
  return (AL_IEncScheduler*)pScheduler;
}

#else

AL_IEncScheduler* AL_SchedulerMcu_Create(AL_TDriver* driver, AL_TAllocator* pDmaAllocator, char const* deviceFile)
{
  (void)driver, (void)pDmaAllocator, (void)deviceFile;
  return NULL;
}

#endif
