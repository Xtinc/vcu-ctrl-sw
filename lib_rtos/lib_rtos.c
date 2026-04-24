#include "lib_rtos/lib_rtos.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void *Rtos_Malloc(size_t zSize)
{
    return malloc(zSize);
}

void Rtos_Free(void *pMem)
{
    free(pMem);
}

void *Rtos_Memcpy(void *pDst, void const *pSrc, size_t zSize)
{
    return memcpy(pDst, pSrc, zSize);
}

void *Rtos_Memmove(void *pDst, void const *pSrc, size_t zSize)
{
    return memmove(pDst, pSrc, zSize);
}

void *Rtos_Memset(void *pDst, int iVal, size_t zSize)
{
    return memset(pDst, iVal, zSize);
}

int Rtos_Memcmp(void const *pBuf1, void const *pBuf2, size_t zSize)
{
    return memcmp(pBuf1, pBuf2, zSize);
}

#if WINDOWS_OS_ENVIRONMENT

#include "windows.h"
#include <limits.h>

#if AL_WAIT_FOREVER != INFINITE
#error ("invalid constant AL_WAIT_FOREVER")
#endif

AL_64U Rtos_GetTime(void)
{
    LARGE_INTEGER counter = {0};
    LARGE_INTEGER freq = {0};

    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&freq);

    if (freq.QuadPart <= 0)
        return 0;

    return (AL_64U)((counter.QuadPart * 1000000ULL) / freq.QuadPart);
}

void Rtos_Sleep(uint32_t uMillisecond)
{
    Sleep(uMillisecond);
}

AL_MUTEX Rtos_CreateMutex(void)
{
    return (AL_MUTEX)CreateMutex(NULL, false, NULL);
}

void Rtos_DeleteMutex(AL_MUTEX Mutex)
{
    CloseHandle((HANDLE)Mutex);
}

bool Rtos_GetMutex(AL_MUTEX Mutex)
{
    return WaitForSingleObject((HANDLE)Mutex, AL_WAIT_FOREVER) == WAIT_OBJECT_0;
}

bool Rtos_ReleaseMutex(AL_MUTEX Mutex)
{
    return ReleaseMutex((HANDLE)Mutex);
}

AL_SEMAPHORE Rtos_CreateSemaphore(int iInitialCount)
{
    return (AL_SEMAPHORE)CreateSemaphore(NULL, iInitialCount, LONG_MAX, NULL);
}

void Rtos_DeleteSemaphore(AL_SEMAPHORE Semaphore)
{
    CloseHandle((HANDLE)Semaphore);
}

bool Rtos_GetSemaphore(AL_SEMAPHORE Semaphore, uint32_t Wait)
{
    return WaitForSingleObject((HANDLE)Semaphore, Wait) == WAIT_OBJECT_0;
}

bool Rtos_ReleaseSemaphore(AL_SEMAPHORE Semaphore)
{
    return ReleaseSemaphore((HANDLE)Semaphore, 1, NULL);
}

AL_EVENT Rtos_CreateEvent(bool bInitialState)
{
    return (AL_EVENT)CreateEvent(NULL, FALSE, bInitialState, NULL);
}

void Rtos_DeleteEvent(AL_EVENT Event)
{
    CloseHandle((HANDLE)Event);
}

bool Rtos_WaitEvent(AL_EVENT Event, uint32_t Wait)
{
    return WaitForSingleObject((HANDLE)Event, Wait) == WAIT_OBJECT_0;
}

bool Rtos_SetEvent(AL_EVENT Event)
{
    return SetEvent((HANDLE)Event);
}

struct AL_WindowsThread
{
    HANDLE handle;
    void *(*func)(void *);
    void *param;
};

static HANDLE GetNative(AL_THREAD Thread)
{
    struct AL_WindowsThread *pThread = (struct AL_WindowsThread *)Thread;
    return pThread->handle;
}

static DWORD WINAPI WindowsCallback(void *p)
{
    struct AL_WindowsThread *pThread = (struct AL_WindowsThread *)p;
    pThread->func(pThread->param);
    return 0;
}

AL_THREAD Rtos_CreateThread(void *(*pFunc)(void *pParam), void *pParam)
{
    struct AL_WindowsThread *pThread = Rtos_Malloc(sizeof(*pThread));
    DWORD id;

    if (!pThread)
        return NULL;

    pThread->func = pFunc;
    pThread->param = pParam;
    pThread->handle = CreateThread(NULL, 0, WindowsCallback, pThread, 0, &id);

    if (!pThread->handle)
    {
        Rtos_Free(pThread);
        return NULL;
    }

    return pThread;
}

void Rtos_SetCurrentThreadName(const char *pThreadName)
{
    (void)pThreadName;
}

bool Rtos_JoinThread(AL_THREAD Thread)
{
    if (!Thread)
        return false;

    DWORD uRet = WaitForSingleObject(GetNative(Thread), INFINITE);
    return uRet == WAIT_OBJECT_0;
}

void Rtos_DeleteThread(AL_THREAD Thread)
{
    if (!Thread)
        return;

    CloseHandle(GetNative(Thread));
    Rtos_Free(Thread);
}

void *Rtos_DriverOpen(char const *name)
{
    (void)name;
    return NULL; // not implemented
}

void Rtos_DriverClose(void *drv)
{
    (void)drv;
    // not implemented
}

int Rtos_DriverIoctl(void *drv, unsigned long int req, void *data)
{
    (void)drv;
    (void)req;
    (void)data;
    return -1; // not implemented
}

int Rtos_DriverPoll(void *drv, Rtos_PollCtx *ctx)
{
    (void)drv, (void)ctx;
    return -1; // not implemented
}

Rtos_AtomicInt Rtos_AtomicIncrement(Rtos_AtomicInt *iVal)
{
    return InterlockedIncrement(iVal);
}

Rtos_AtomicInt Rtos_AtomicDecrement(Rtos_AtomicInt *iVal)
{
    return InterlockedDecrement(iVal);
}

#elif LINUX_OS_ENVIRONMENT

#include <errno.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <sys/ioctl.h>

typedef struct
{
    pthread_mutex_t Mutex;
    pthread_cond_t Cond;
    bool bSignaled;
} evt_t;

static void AddMsToTimespec(struct timespec *ts, uint32_t waitMs)
{
    ts->tv_sec += waitMs / 1000;
    ts->tv_nsec += (long)((waitMs % 1000) * 1000000L);

    if (ts->tv_nsec >= 1000000000L)
    {
        ts->tv_sec += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

AL_64U Rtos_GetTime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);

    return ((AL_64U)ts.tv_sec) * 1000000 + (ts.tv_nsec / 1000);
}

void Rtos_Sleep(uint32_t uMillisecond)
{
    struct timespec requested = {
        .tv_sec = (time_t)(uMillisecond / 1000),
        .tv_nsec = (long)((uMillisecond % 1000) * 1000000L),
    };

    while (nanosleep(&requested, &requested) == -1 && errno == EINTR)
    {
    }
}

AL_MUTEX Rtos_CreateMutex(void)
{
    pthread_mutex_t *pMutex = (pthread_mutex_t *)Rtos_Malloc(sizeof(*pMutex));

    if (pMutex)
    {
        pthread_mutexattr_t MutexAttr;

        if (pthread_mutexattr_init(&MutexAttr) != 0)
        {
            Rtos_Free(pMutex);
            return NULL;
        }

        if (pthread_mutexattr_settype(&MutexAttr, PTHREAD_MUTEX_RECURSIVE) != 0 ||
            pthread_mutex_init(pMutex, &MutexAttr) != 0)
        {
            pthread_mutexattr_destroy(&MutexAttr);
            Rtos_Free(pMutex);
            return NULL;
        }

        pthread_mutexattr_destroy(&MutexAttr);
    }

    return (AL_MUTEX)pMutex;
}

void Rtos_DeleteMutex(AL_MUTEX Mutex)
{
    pthread_mutex_t *pMutex = (pthread_mutex_t *)Mutex;

    if (pMutex)
    {
        pthread_mutex_destroy(pMutex);
        Rtos_Free(pMutex);
    }
}

bool Rtos_GetMutex(AL_MUTEX Mutex)
{
    pthread_mutex_t *pMutex = (pthread_mutex_t *)Mutex;

    if (!pMutex)
        return false;

    return pthread_mutex_lock(pMutex) == 0;
}

bool Rtos_ReleaseMutex(AL_MUTEX Mutex)
{
    if (!Mutex)
        return false;

    return pthread_mutex_unlock((pthread_mutex_t *)Mutex) == 0;
}

AL_SEMAPHORE Rtos_CreateSemaphore(int iInitialCount)
{
    sem_t *pSem = (sem_t *)Rtos_Malloc(sizeof(*pSem));

    if (pSem && sem_init(pSem, 0, iInitialCount) != 0)
    {
        Rtos_Free(pSem);
        return NULL;
    }

    return (AL_SEMAPHORE)pSem;
}

void Rtos_DeleteSemaphore(AL_SEMAPHORE Semaphore)
{
    sem_t *pSem = (sem_t *)Semaphore;

    if (pSem)
    {
        sem_destroy(pSem);
        Rtos_Free(pSem);
    }
}

bool Rtos_GetSemaphore(AL_SEMAPHORE Semaphore, uint32_t Wait)
{
    sem_t *pSem = (sem_t *)Semaphore;

    if (!pSem)
        return false;

    int ret;

    if (Wait == AL_NO_WAIT)
    {
        do
        {
            ret = sem_trywait(pSem);
        } while (ret == -1 && errno == EINTR);

        return ret == 0;
    }
    else if (Wait == AL_WAIT_FOREVER)
    {
        do
        {
            ret = sem_wait(pSem);
        } while (ret == -1 && errno == EINTR);

        return ret == 0;
    }
    else
    {
        struct timespec deadline;

        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
            return false;

        AddMsToTimespec(&deadline, Wait);

        do
        {
            ret = sem_timedwait(pSem, &deadline);
        } while (ret == -1 && errno == EINTR);

        return ret == 0;
    }
}

bool Rtos_ReleaseSemaphore(AL_SEMAPHORE Semaphore)
{
    sem_t *pSem = (sem_t *)Semaphore;

    if (!pSem)
        return false;

    return sem_post(pSem) == 0;
}

AL_EVENT Rtos_CreateEvent(bool bInitialState)
{
    evt_t *pEvt = (evt_t *)Rtos_Malloc(sizeof(evt_t));

    if (!pEvt)
        return NULL;

    if (pthread_mutex_init(&pEvt->Mutex, NULL) != 0)
    {
        Rtos_Free(pEvt);
        return NULL;
    }

    if (pthread_cond_init(&pEvt->Cond, NULL) != 0)
    {
        pthread_mutex_destroy(&pEvt->Mutex);
        Rtos_Free(pEvt);
        return NULL;
    }

    pEvt->bSignaled = bInitialState;

    return (AL_EVENT)pEvt;
}

void Rtos_DeleteEvent(AL_EVENT Event)
{
    evt_t *pEvt = (evt_t *)Event;

    if (pEvt)
    {
        pthread_cond_destroy(&pEvt->Cond);
        pthread_mutex_destroy(&pEvt->Mutex);
    }
    Rtos_Free(pEvt);
}

bool Rtos_WaitEvent(AL_EVENT Event, uint32_t Wait)
{
    evt_t *pEvt = (evt_t *)Event;

    if (!pEvt)
        return false;

    bool reachedDeadline = false;
    bool lockOk = pthread_mutex_lock(&pEvt->Mutex) == 0;

    if (!lockOk)
        return false;

    if (Wait == AL_NO_WAIT)
    {
        reachedDeadline = !pEvt->bSignaled;
    }
    else if (Wait == AL_WAIT_FOREVER)
    {
        while (!pEvt->bSignaled)
            pthread_cond_wait(&pEvt->Cond, &pEvt->Mutex);
    }
    else
    {
        struct timespec deadline;

        if (clock_gettime(CLOCK_REALTIME, &deadline) != 0)
            reachedDeadline = true;

        if (!reachedDeadline)
            AddMsToTimespec(&deadline, Wait);

        while (!reachedDeadline && !pEvt->bSignaled)
        {
            int waitRet = pthread_cond_timedwait(&pEvt->Cond, &pEvt->Mutex, &deadline);

            if (waitRet == ETIMEDOUT)
                reachedDeadline = true;
            else if (waitRet != 0)
            {
                pthread_mutex_unlock(&pEvt->Mutex);
                return false;
            }
        }
    }

    if (!reachedDeadline)
        pEvt->bSignaled = false;

    pthread_mutex_unlock(&pEvt->Mutex);
    return !reachedDeadline;
}

bool Rtos_SetEvent(AL_EVENT Event)
{
    evt_t *pEvt = (evt_t *)Event;

    if (!pEvt)
        return false;

    pthread_mutex_lock(&pEvt->Mutex);
    pEvt->bSignaled = true;
    bool bRet = pthread_cond_signal(&pEvt->Cond) == 0;
    pthread_mutex_unlock(&pEvt->Mutex);
    return bRet;
}

static pthread_t GetNative(AL_THREAD Thread)
{
    return *((pthread_t *)Thread);
}

AL_THREAD Rtos_CreateThread(void *(*pFunc)(void *pParam), void *pParam)
{
    pthread_t *thread = Rtos_Malloc(sizeof(pthread_t));

    if (thread && pthread_create(thread, NULL, pFunc, pParam) != 0)
    {
        Rtos_Free(thread);
        return NULL;
    }

    return (AL_THREAD)thread;
}

void Rtos_SetCurrentThreadName(const char *pThreadName)
{
    prctl(PR_SET_NAME, (unsigned long)pThreadName, 0, 0, 0);
}

bool Rtos_JoinThread(AL_THREAD Thread)
{
    if (!Thread)
        return false;

    int iRet = pthread_join(GetNative(Thread), NULL);
    return iRet == 0;
}

void Rtos_DeleteThread(AL_THREAD Thread)
{
    Rtos_Free((pthread_t *)Thread);
}

void *Rtos_DriverOpen(char const *name)
{
    int fd = open(name, O_RDWR | O_NONBLOCK);

    if (fd == -1)
        return NULL;
    return (void *)(intptr_t)fd;
}

void Rtos_DriverClose(void *drv)
{
    int fd = (int)(intptr_t)drv;
    close(fd);
}

int Rtos_DriverIoctl(void *drv, unsigned long int req, void *data)
{
    int fd = (int)(intptr_t)drv;
    return ioctl(fd, req, data);
}

int Rtos_DriverPoll(void *drv, Rtos_PollCtx *ctx)
{
    struct pollfd pollData;
    /* bitfield are bit compatible */
    pollData.events = ctx->events;
    pollData.fd = (int)(intptr_t)drv;

    int err = poll(&pollData, 1, ctx->timeout);

    if (err == -1 || err == 0)
        return err;

    ctx->revents = pollData.revents;
    return 1;
}

Rtos_AtomicInt Rtos_AtomicIncrement(Rtos_AtomicInt *iVal)
{
    return __sync_add_and_fetch(iVal, 1);
}

Rtos_AtomicInt Rtos_AtomicDecrement(Rtos_AtomicInt *iVal)
{
    return __sync_sub_and_fetch(iVal, 1);
}

#else
#error "Unsupported OS environment"
#endif

static void *pCacheCBCtx;
static Rtos_MemoryFnCB pfnInvalMemoryCB = NULL;
static Rtos_MemoryFnCB pfnFlushMemoryCB = NULL;

void Rtos_InitCacheCB(void *ctx, Rtos_MemoryFnCB pfnInvalCB, Rtos_MemoryFnCB pfnFlushCB)
{
    pCacheCBCtx = ctx;
    pfnInvalMemoryCB = pfnInvalCB;
    pfnFlushMemoryCB = pfnFlushCB;
}

void Rtos_InvalidateCacheMemory(void *pMem, size_t zSize)
{
    if (pfnInvalMemoryCB)
        pfnInvalMemoryCB(pCacheCBCtx, pMem, zSize);
}

void Rtos_FlushCacheMemory(void *pMem, size_t zSize)
{
    if (pfnFlushMemoryCB)
        pfnFlushMemoryCB(pCacheCBCtx, pMem, zSize);
}
