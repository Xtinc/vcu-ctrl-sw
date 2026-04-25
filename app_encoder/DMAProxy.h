#ifndef DMA_PROXY_H
#define DMA_PROXY_H

extern "C"
{
#include "lib_common/BufferAPI.h"
}

class DMAProxy
{
  public:
    explicit DMAProxy(const char *device);
    ~DMAProxy();
    void move(AL_TBuffer *destination, int destination_offset, AL_TBuffer const *source, int source_offset,
              size_t size);
    void set(AL_TBuffer *destination, int destination_offset, int value, size_t size);

  private:
    int m_fd;
};

#endif // DMA_PROXY_H