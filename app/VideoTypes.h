#ifndef VIDEO_TYPES_H
#define VIDEO_TYPES_H

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

enum class VideoCodec : uint8_t
{
    HEVC = 0, ///< H.265 / HEVC
    AVC = 1,  ///< H.264 / AVC
};
enum class RateControl : uint8_t
{
    CBR = 0, ///< Constant bitrate
    VBR = 1, ///< Variable bitrate (target + peak)
    CQP = 2, ///< Constant QP
};

enum class StreamFlags : uint8_t
{
    Unknown = 0x0,    ///< Content type not specified (slice boundary unknown)
    EndOfSlice = 0x2, ///< Buffer ends a slice
    EndOfFrame = 0x4, ///< Buffer ends a complete frame
};

struct SLICHead
{
    uint32_t slice_tok : 4;
    uint32_t slice_num : 4;
    uint32_t frame_idx : 24;
};

inline size_t pack_slice(uint8_t *dst, const uint8_t *payload, size_t payload_length, uint32_t frame_idx,
                         uint8_t slice_tok, uint8_t slice_num)
{
    SLICHead head{slice_tok, slice_num, frame_idx};
    std::memcpy(dst, &head, sizeof(head));
    std::memcpy(dst + sizeof(head), payload, payload_length);
    return sizeof(head) + payload_length;
}

#endif // VIDEO_TYPES_H