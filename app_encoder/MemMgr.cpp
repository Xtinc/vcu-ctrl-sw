#include "MemMgr.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#if LINUX_OS_ENVIRONMENT
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C"
{
#include "lib_fpga/DmaAlloc.h"
#include "lib_fpga/DmaAllocLinux.h"
}

typedef struct dmaproxy_arg
{
    int src_fd;
    int dst_fd;
    int src_offset;
    int dst_offset;
    size_t size;
} dmaproxy_arg_t;

#define DMAPROXY_IOCTL_MAGIC 0x32
#define DMAPROXY_COPY _IOWR(DMAPROXY_IOCTL_MAGIC, 1, dmaproxy_arg_t *)

DMAProxy::DMAProxy(const char *device)
{
    m_fd = ::open(device, O_RDWR);
    if (m_fd < 0)
    {
        VIDEO_ERROR_PRINT("Failed to open DMA proxy device: %s", device);
    }
}

DMAProxy::~DMAProxy()
{
    if (m_fd >= 0)
    {
        ::close(m_fd);
    }
}

void DMAProxy::move(AL_TBuffer *destination, int destination_offset, AL_TBuffer const *source, int source_offset,
                    size_t size)
{
    if (m_fd < 0)
    {
        std::move(AL_Buffer_GetData(source) + source_offset, AL_Buffer_GetData(source) + source_offset + size,
                  AL_Buffer_GetData(destination) + destination_offset);
        return;
    }

    auto src_allocator = source->pAllocator;
    int src_fd = AL_LinuxDmaAllocator_GetFd((AL_TLinuxDmaAllocator *)src_allocator, source->hBufs[0]);

    auto dst_allocator = destination->pAllocator;
    int dst_fd = AL_LinuxDmaAllocator_GetFd((AL_TLinuxDmaAllocator *)dst_allocator, destination->hBufs[0]);

    dmaproxy_arg_t dmaproxy{};
    dmaproxy.size = size;
    dmaproxy.dst_offset = destination_offset;
    dmaproxy.src_offset = source_offset;
    dmaproxy.src_fd = src_fd;
    dmaproxy.dst_fd = dst_fd;

    if (::ioctl(m_fd, DMAPROXY_COPY, &dmaproxy) < 0)
    {
        VIDEO_ERROR_PRINT("DMA copy failed, falling back to CPU move: %s", ::strerror(errno));
        std::move(AL_Buffer_GetData(source) + source_offset, AL_Buffer_GetData(source) + source_offset + size,
                  AL_Buffer_GetData(destination) + destination_offset);
    }
}

#else
DMAProxy::DMAProxy(const char *device) : m_fd(-1)
{
}

DMAProxy::~DMAProxy() = default;

void DMAProxy::move(AL_TBuffer *destination, int destination_offset, AL_TBuffer const *source, int source_offset,
                    size_t size)
{
    std::move(AL_Buffer_GetData(source) + source_offset, AL_Buffer_GetData(source) + source_offset + size,
              AL_Buffer_GetData(destination) + destination_offset);
}

#endif

void DMAProxy::set(AL_TBuffer *destination, int destination_offset, int value, size_t size)
{
    std::memset(AL_Buffer_GetData(destination) + destination_offset, value, size);
}

BufPool::~BufPool()
{
    for (auto *pBuf : m_pool)
    {
        if (!pBuf)
        {
            continue;
        }

        AL_Buffer_SetUserData(pBuf, nullptr);
        AL_Buffer_Destroy(pBuf);
    }
}

bool BufPool::init(AL_TAllocator *pAllocator, uint32_t uNumBuf)
{
    if (!pAllocator || uNumBuf == 0)
    {
        return false;
    }

    m_pAllocator = pAllocator;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_pool.clear();
        m_fifo.clear();
        m_capacity = uNumBuf;
        m_decommitted = false;
    }

    m_pool.reserve(uNumBuf);

    while (m_pool.size() < uNumBuf)
    {
        if (!add_alloc_buf())
        {
            return false;
        }
    }

    return true;
}

bool BufPool::add_metadata(AL_TMetaData *pMetaData)
{
    for (auto *pBuf : m_pool)
    {
        auto *pMeta = AL_MetaData_Clone(pMetaData);
        if (!AL_Buffer_AddMetaData(pBuf, pMeta))
        {
            return false;
        }
    }
    return true;
}

AL_TBuffer *BufPool::get_buffer(bool block)
{
    std::unique_lock<std::mutex> lock(m_mutex);

    if (block)
    {
        m_cond.wait(lock, [this] { return !m_fifo.empty() || m_decommitted; });
        if (m_fifo.empty())
        {
            return nullptr;
        }
    }
    else
    {
        if (m_fifo.empty())
        {
            return nullptr;
        }
    }

    auto *pBuf = m_fifo.front();
    m_fifo.pop_front();
    lock.unlock();
    AL_Buffer_Ref(pBuf);
    return pBuf;
}

size_t BufPool::available_count()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_fifo.size();
}

void BufPool::commit()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_decommitted = true;
    }
    m_cond.notify_all();
}

void BufPool::decommit()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_decommitted = false;
}

BufPool::BufPool() : m_pAllocator(nullptr), m_capacity(0), m_decommitted(false)
{
}

void BufPool::s_free_buf_in_pool(AL_TBuffer *pBuf)
{
    auto *pBufPool = static_cast<BufPool *>(AL_Buffer_GetUserData(pBuf));

    if (!pBufPool)
    {
        return;
    }

    pBufPool->queue_buf(pBuf);
}

bool BufPool::add_alloc_buf()
{
    auto *pBuf = create_buf(m_pAllocator, &BufPool::s_free_buf_in_pool);

    if (!pBuf)
    {
        return false;
    }

    AL_Buffer_SetUserData(pBuf, this);
    m_pool.push_back(pBuf);
    queue_buf(pBuf);
    return true;
}

void BufPool::queue_buf(AL_TBuffer *pBuf)
{
    std::unique_lock<std::mutex> lock(m_mutex);
    m_fifo.push_back(pBuf);
    lock.unlock();
    m_cond.notify_one();
}

bool GenericBufPool::init(AL_TAllocator *pAllocator, uint32_t uNumBuf, size_t zBufSize, AL_TMetaData *pMetaData,
                          const std::string &sName)
{
    m_buf_num = uNumBuf;
    m_buf_size = zBufSize;
    m_name = sName;

    if (!BufPool::init(pAllocator, uNumBuf))
    {
        return false;
    }

    return pMetaData == nullptr || add_metadata(pMetaData);
}

AL_TBuffer *GenericBufPool::create_buf(AL_TAllocator *pAllocator, PFN_RefCount_CallBack pRefCntCallBack)
{
    return AL_Buffer_Create_And_AllocateNamed(pAllocator, m_buf_size, pRefCntCallBack, m_name.c_str());
}

size_t GenericBufPool::get_buf_size() const
{
    return m_buf_size;
}

uint32_t GenericBufPool::get_num_buf() const
{
    return m_buf_num;
}

void PixMapBufPool::set_format(AL_TDimension tDim, TFourCC tFourCC)
{
    m_tDim = tDim;
    m_tFourCC = tFourCC;
}

void PixMapBufPool::add_chunk(size_t zSize, const std::vector<AL_TPlaneDescription> &vPlDescriptions)
{
    m_plane_chunks.push_back(PlaneChunk{zSize, vPlDescriptions});
}

bool PixMapBufPool::init(AL_TAllocator *pAllocator, uint32_t uNumBuf, const std::string &sName)
{
    m_name = sName;
    return BufPool::init(pAllocator, uNumBuf);
}

AL_TBuffer *PixMapBufPool::create_buf(AL_TAllocator *pAllocator, PFN_RefCount_CallBack pRefCntCallBack)
{
    auto *pBuf = AL_PixMapBuffer_Create(pAllocator, pRefCntCallBack, m_tDim, m_tFourCC);
    if (pBuf == nullptr)
    {
        return nullptr;
    }

    for (const auto &chunk : m_plane_chunks)
    {
        const auto *pPlaneDescriptions = chunk.pl_descs.empty() ? nullptr : chunk.pl_descs.data();

        if (!AL_PixMapBuffer_Allocate_And_AddPlanes(pBuf, chunk.sz, pPlaneDescriptions, chunk.pl_descs.size(),
                                                    m_name.c_str()))
        {
            AL_Buffer_Destroy(pBuf);
            return nullptr;
        }
    }

    return pBuf;
}
