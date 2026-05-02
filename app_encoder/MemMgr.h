#ifndef VIDEO_MEMORY_MANAGER_H
#define VIDEO_MEMORY_MANAGER_H

extern "C"
{
#include "lib_common/Allocator.h"
#include "lib_common/BufferAPI.h"
#include "lib_common/BufferMeta.h"
#include "lib_common/PixMapBuffer.h"
}

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

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

class BufPool
{
  public:
    virtual ~BufPool();

    BufPool(const BufPool &) = delete;
    BufPool &operator=(const BufPool &) = delete;
    BufPool(BufPool &&) = delete;
    BufPool &operator=(BufPool &&) = delete;

    bool init(AL_TAllocator *pAllocator, uint32_t uNumBuf);
    bool add_metadata(AL_TMetaData *pMetaData);
    AL_TBuffer *get_buffer(bool block = true);
    size_t available_count();
    void commit();
    void decommit();

    virtual AL_TBuffer *create_buf(AL_TAllocator *pAllocator, PFN_RefCount_CallBack pRefCntCallBack) = 0;

  protected:
    BufPool();

  private:
    static void s_free_buf_in_pool(AL_TBuffer *pBuf);
    bool add_alloc_buf();
    void queue_buf(AL_TBuffer *pBuf);

  private:
    AL_TAllocator *m_pAllocator;
    std::vector<AL_TBuffer *> m_pool;
    std::deque<AL_TBuffer *> m_fifo;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    uint32_t m_capacity;
    bool m_decommitted;
};

class GenericBufPool : public BufPool
{
  public:
    bool init(AL_TAllocator *pAllocator, uint32_t uNumBuf, size_t zBufSize, AL_TMetaData *pMetaData,
              const std::string &sName);
    AL_TBuffer *create_buf(AL_TAllocator *pAllocator, PFN_RefCount_CallBack pRefCntCallBack) override;
    size_t get_buf_size() const;
    uint32_t get_num_buf() const;

  private:
    uint32_t m_buf_num;
    size_t m_buf_size;
    std::string m_name;
};

class PixMapBufPool : public BufPool
{
  public:
    void set_format(AL_TDimension tDim, TFourCC tFourCC);
    void add_chunk(size_t zSize, const std::vector<AL_TPlaneDescription> &vPlDescriptions);
    bool init(AL_TAllocator *pAllocator, uint32_t uNumBuf, const std::string &sName);
    AL_TBuffer *create_buf(AL_TAllocator *pAllocator, PFN_RefCount_CallBack pRefCntCallBack) override;

  private:
    struct PlaneChunk
    {
        size_t sz;
        std::vector<AL_TPlaneDescription> pl_descs;
    };

    std::vector<PlaneChunk> m_plane_chunks;
    AL_TDimension m_tDim;
    TFourCC m_tFourCC;
    std::string m_name;
};

#endif // MEMORY_MANAGER_H