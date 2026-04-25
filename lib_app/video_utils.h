#ifndef VIDEO_UTILS_H
#define VIDEO_UTILS_H

#include <cinttypes>
#include <string>


inline std::string FourCC2STR(uint32_t fourcc)
{
    char str[5] = {};
    str[0] = static_cast<char>((fourcc >> 0) & 0xFF);
    str[1] = static_cast<char>((fourcc >> 8) & 0xFF);
    str[2] = static_cast<char>((fourcc >> 16) & 0xFF);
    str[3] = static_cast<char>((fourcc >> 24) & 0xFF);
    return std::string(str);
}

#endif // VIDEO_UTILS_H