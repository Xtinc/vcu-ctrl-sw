#ifndef DMA_BUFFER_DESC_H
#define DMA_BUFFER_DESC_H

extern "C"
{
#include "lib_common/FourCC.h"
}

#include <cstdint>
#include <string>

struct DMAFd
{
    int dma_fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    TFourCC fourcc = 0;
    uint32_t y_offset = 0;
    uint32_t y_pitch = 0;
    uint32_t uv_offset = 0;
    uint32_t uv_pitch = 0;
};

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