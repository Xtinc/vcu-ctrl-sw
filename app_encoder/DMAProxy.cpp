#include "DMAProxy.h"
#include "lib_rtos/message.h"
#include <cstring>

#if LINUX_OS_ENVIRONMENT
#include <algorithm>
#include <cerrno>
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