#ifndef UDP_MEM_H
#define UDP_MEM_H

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <vector>

extern "C"
{
#include "lib_rtos/message.h"
}

template <size_t CHUNK_MIN_ORD, size_t CHUNK_MAX_ORD> class MemPool
{
    using ChunkList = std::list<uint8_t *>;

    static_assert(CHUNK_MIN_ORD < CHUNK_MAX_ORD, "CHUNK_MIN_ORD must be less than CHUNK_MAX_ORD");
    static_assert(CHUNK_MIN_ORD >= 6, "CHUNK_MIN_ORD must be greater than 6");
    static_assert(CHUNK_MAX_ORD <= 17, "CHUNK_MAX_ORD must be less than or equal to 17");
    static constexpr size_t MAX_MEMPOOL_SZ = 2 * 1024 * 1024 * 1024ULL; // 2GB

  public:
    MemPool(size_t initial_blocks) : current_size(0)
    {
        for (size_t i = CHUNK_MIN_ORD; i <= CHUNK_MAX_ORD; ++i)
        {
            expand_memory((static_cast<size_t>(1) << i) + sizeof(size_t), initial_blocks, i - CHUNK_MIN_ORD);
        }
    }

    ~MemPool()
    {
        for (auto &region : memory_regions_)
        {
            delete[] region;
        }
    }

    MemPool(const MemPool &) = delete;
    MemPool &operator=(const MemPool &) = delete;

    void *allocate(size_t size, bool must_allocate = true)
    {
        size_t ord = CHUNK_MIN_ORD;
        while ((static_cast<size_t>(1) << ord) < size && ord <= CHUNK_MAX_ORD)
        {
            ord++;
        }

        if (ord > CHUNK_MAX_ORD)
        {
            VIDEO_ERROR_PRINT("Requested size %zu exceeds maximum manageable size", size);
            return nullptr;
        }

        size_t chunk_idx = ord - CHUNK_MIN_ORD;
        if (chunks_[chunk_idx].empty())
        {
            if (!must_allocate && current_size > MAX_MEMPOOL_SZ)
            {
                return nullptr;
            }

            expand_memory((static_cast<size_t>(1) << ord) + sizeof(size_t), 25, chunk_idx);
            VIDEO_INFO_PRINT("Expanded memory chunk %.2f MB", static_cast<double>(current_size) / (1024.0 * 1024.0));
        }

        uint8_t *block = chunks_[chunk_idx].front();
        *reinterpret_cast<size_t *>(block) = size;
        chunks_[chunk_idx].pop_front();
        return static_cast<void *>(block + sizeof(size_t));
    }

    void deallocate(void *ptr)
    {
        if (!ptr)
        {
            return;
        }

        auto *block = static_cast<uint8_t *>(ptr);
        size_t block_size = *reinterpret_cast<size_t *>(block - sizeof(size_t));
        size_t ord = CHUNK_MIN_ORD;

        while ((static_cast<size_t>(1) << ord) < block_size && ord <= CHUNK_MAX_ORD)
        {
            ord++;
        }

        if (ord > CHUNK_MAX_ORD)
        {
            VIDEO_ERROR_PRINT("Invalid block size during deallocation: %zu", block_size);
            return;
        }

        size_t chunk_idx = ord - CHUNK_MIN_ORD;
        chunks_[chunk_idx].push_front(block - sizeof(size_t));
    }

  private:
    void expand_memory(size_t block_size, size_t count, size_t chunk_idx)
    {
        auto *region_ptr = new uint8_t[block_size * count];
        for (size_t i = 0; i < count; ++i)
        {
            auto *block = region_ptr + i * block_size;
            chunks_[chunk_idx].push_back(block);
        }
        memory_regions_.push_back(region_ptr);
        current_size += block_size * count;
    }

  private:
    std::array<ChunkList, CHUNK_MAX_ORD - CHUNK_MIN_ORD + 1> chunks_;
    std::vector<uint8_t *> memory_regions_;
    size_t current_size;
};

#endif // UDP_MEM_H