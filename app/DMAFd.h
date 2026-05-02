#ifndef DMA_BUFFER_DESC_H
#define DMA_BUFFER_DESC_H

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_common/FourCC.h"
}

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct DMAFd
{
    DMAFd();
    DMAFd(int fd, AL_TBuffer *buf, uint32_t w, uint32_t h, AL_TPicFormat pic_fmt);
    ~DMAFd();

    // Disable copy semantics, allow move semantics
    DMAFd(const DMAFd &) = delete;
    DMAFd &operator=(const DMAFd &) = delete;
    DMAFd(DMAFd &&other) noexcept;
    DMAFd &operator=(DMAFd &&other) noexcept;

    int dma_fd;
    AL_TBuffer *buffer;
    uint32_t width;
    uint32_t height;
    TFourCC fourcc;
    uint32_t y_offset;
    uint32_t y_pitch;
    uint32_t uv_offset;
    uint32_t uv_pitch;
};

using DMAFdArray = std::vector<DMAFd>;

inline std::string FOURCC2STR(TFourCC fourcc)
{
    char buf[5]{};
    buf[0] = static_cast<char>(fourcc & 0xFF);
    buf[1] = static_cast<char>((fourcc >> 8) & 0xFF);
    buf[2] = static_cast<char>((fourcc >> 16) & 0xFF);
    buf[3] = static_cast<char>((fourcc >> 24) & 0xFF);
    return std::string(buf);
}

inline TFourCC STR2FOURCC(const std::string &str)
{
    if (str.size() != 4)
    {
        return 0;
    }
    return static_cast<uint32_t>(str[0]) | (static_cast<uint32_t>(str[1]) << 8) |
           (static_cast<uint32_t>(str[2]) << 16) | (static_cast<uint32_t>(str[3]) << 24);
}

#endif // DMA_BUFFER_DESC_H