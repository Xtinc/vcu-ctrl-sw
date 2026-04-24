#pragma once

#include <inttypes.h>
#include <stdalign.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32) || defined(_WIN64)
#include <sdkddkver.h>
#if _WIN32_WINNT >= 0x0600 // Windows Vista
#define WINDOWS_OS_ENVIRONMENT 1
#else
#define WINDOWS_OS_ENVIRONMENT 0
#endif
#else
#define WINDOWS_OS_ENVIRONMENT 0
#endif

#if defined(__linux__) || defined(__linux) || defined(linux)
#define LINUX_OS_ENVIRONMENT 1
#else
#define LINUX_OS_ENVIRONMENT 0
#endif

#if WINDOWS_OS_ENVIRONMENT
#define __AL_ALIGNED__(x) __declspec(align(x))
#else
#define __AL_ALIGNED__(x) __attribute__((__aligned__(x)))
#endif

typedef uint64_t __AL_ALIGNED__(8) AL_64U;
typedef int64_t  __AL_ALIGNED__(8) AL_64S;
typedef uint8_t *AL_VADDR; /*!< Virtual address. byte pointer */
typedef uint32_t AL_PADDR; /**< @brief Physical address, 32-bit address registers */
typedef AL_64U AL_PTR64;
typedef void *AL_HANDLE;
typedef void const *AL_CONST_HANDLE;

typedef void *AL_MUTEX;     /**< @brief Mutex handle */
typedef void *AL_SEMAPHORE; /**< @brief Semaphore handle */
typedef void *AL_EVENT;     /**< @brief Event handle */
typedef void *AL_THREAD;    /**< @brief Thread handle */

#define AL_NO_WAIT 0
#define AL_WAIT_FOREVER 0xFFFFFFFF

// Memory
void *Rtos_Malloc(size_t zSize);
void Rtos_Free(void *pMem);

void *Rtos_Memcpy(void *pDst, void const *pSrc, size_t zSize);
void *Rtos_Memmove(void *pDst, void const *pSrc, size_t zSize);
void *Rtos_Memset(void *pDst, int iVal, size_t zSize);
int Rtos_Memcmp(void const *pBuf1, void const *pBuf2, size_t zSize);

// Clock
/**
 * @brief Return the time in microseconds (us)
 */
AL_64U Rtos_GetTime(void);
void Rtos_Sleep(uint32_t uMillisecond);

// Mutex
AL_MUTEX Rtos_CreateMutex(void);
void Rtos_DeleteMutex(AL_MUTEX Mutex);
bool Rtos_GetMutex(AL_MUTEX Mutex);
bool Rtos_ReleaseMutex(AL_MUTEX Mutex);

// Semaphore
AL_SEMAPHORE Rtos_CreateSemaphore(int iInitialCount);
void Rtos_DeleteSemaphore(AL_SEMAPHORE Semaphore);
bool Rtos_GetSemaphore(AL_SEMAPHORE Semaphore, uint32_t Wait);
bool Rtos_ReleaseSemaphore(AL_SEMAPHORE Semaphore);

// Event
AL_EVENT Rtos_CreateEvent(bool iInitialState);
void Rtos_DeleteEvent(AL_EVENT Event);
bool Rtos_WaitEvent(AL_EVENT Event, uint32_t Wait);
bool Rtos_SetEvent(AL_EVENT Event);

// Threads
AL_THREAD Rtos_CreateThread(void *(*pFunc)(void *pParam), void *pParam);
void Rtos_SetCurrentThreadName(const char *pThreadName);
bool Rtos_JoinThread(AL_THREAD Thread);
void Rtos_DeleteThread(AL_THREAD Thread);

// Driver
void *Rtos_DriverOpen(char const *name);
void Rtos_DriverClose(void *drv);
int Rtos_DriverIoctl(void *drv, unsigned long int req, void *data);

#define AL_POLLIN 0x001  /* There is data to read.  */
#define AL_POLLPRI 0x002 /* There is urgent data to read.  */
#define AL_POLLOUT 0x004 /* Writing now will not block.  */

// Event types always implicitly polled for.  These bits need not be set in
// `events', but they will appear in `revents' to indicate the status of
// the file descriptor.
#define AL_POLLERR 0x008  /* Error condition.  */
#define AL_POLLHUP 0x010  /* Hung up.  */
#define AL_POLLNVAL 0x020 /* Invalid polling request.  */

typedef struct Rtos_PollCtx_t
{
    unsigned long events;
    unsigned long revents;
    int timeout;
} Rtos_PollCtx;

int Rtos_DriverPoll(void *drv, Rtos_PollCtx *ctx);

// Atomics
typedef int32_t Rtos_AtomicInt;
Rtos_AtomicInt Rtos_AtomicIncrement(Rtos_AtomicInt *iVal);
Rtos_AtomicInt Rtos_AtomicDecrement(Rtos_AtomicInt *iVal);

// Cache Memory Coherency
typedef void (*Rtos_MemoryFnCB)(void *ctx, void *pMem, size_t zSize);
void Rtos_InitCacheCB(void *ctx, Rtos_MemoryFnCB pfnInvalCB, Rtos_MemoryFnCB pfnFlushCB);
void Rtos_InvalidateCacheMemory(void *pMem, size_t zSize);
void Rtos_FlushCacheMemory(void *pMem, size_t zSize);