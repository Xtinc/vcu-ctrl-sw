// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief SEI timestamp payload structure
 * 
 * UUID: Custom UUID for latency measurement
 * Timestamp: Frame encode time in nanoseconds
 * Frame index: Sequential frame number
 */
typedef struct
{
    uint8_t uuid[16];      // Custom UUID identifier
    uint64_t timestamp_ns; // Encode timestamp (nanoseconds)
    uint64_t frame_index;  // Frame sequence number
} SEI_TimestampPayload;

/**
 * @brief Generate SEI user_data_unregistered NAL unit for latency measurement
 * 
 * Creates a complete SEI NAL unit with timing information embedded in
 * user_data_unregistered message.
 * 
 * @param codec Codec type (0=AVC, 1=HEVC)
 * @param timestamp_ns Frame encode timestamp in nanoseconds
 * @param frame_index Sequential frame number
 * @param out_buffer Output buffer (must be at least SEI_TIMESTAMP_MAX_SIZE bytes)
 * @param out_size Actual size of generated SEI NAL unit
 * @return 0 on success, negative on error
 */
int SEI_GenerateTimestampNAL(
    int codec,              // 0=AVC, 1=HEVC
    uint64_t timestamp_ns,
    uint64_t frame_index,
    uint8_t* out_buffer,
    size_t* out_size);

/**
 * @brief Maximum size of SEI timestamp NAL unit (bytes)
 */
#define SEI_TIMESTAMP_MAX_SIZE 64

/**
 * @brief Custom UUID for latency measurement: a1b2c3d4-e5f6-4789-a1b2-c3d4e5f67890
 */
extern const uint8_t SEI_LATENCY_UUID[16];

#ifdef __cplusplus
}
#endif
