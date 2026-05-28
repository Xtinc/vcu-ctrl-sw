#pragma once

#include <cstdint>

/**
 * @brief Supported video codecs.
 */
enum class VideoCodec : uint8_t
{
    HEVC = 0, ///< H.265 / HEVC
    AVC  = 1, ///< H.264 / AVC
};

/**
 * @brief Rate-control modes for the encoder.
 */
enum class RateControl : uint8_t
{
    CBR = 0, ///< Constant bitrate
    VBR = 1, ///< Variable bitrate (target + peak)
    CQP = 2, ///< Constant QP
};

/**
 * @brief Wire-protocol flags carried in the first byte of every UDP payload.
 *
 * Values are intentionally identical to AL_EStreamBufFlags so they can be
 * cast directly when feeding the hardware decoder.
 */
enum class StreamFlags : uint8_t
{
    Unknown    = 0x0, ///< Content type not specified (slice boundary unknown)
    EndOfSlice = 0x2, ///< Buffer ends a slice
    EndOfFrame = 0x4, ///< Buffer ends a complete frame
};
