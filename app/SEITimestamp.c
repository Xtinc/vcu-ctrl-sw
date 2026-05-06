// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "SEITimestamp.h"
#include <string.h>

// Custom UUID for latency measurement: a1b2c3d4-e5f6-4789-a1b2-c3d4e5f67890
const uint8_t SEI_LATENCY_UUID[16] = {
    0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x47, 0x89,
    0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x78, 0x90
};

// Helper: Write uint64_t in big-endian
static void write_u64_be(uint8_t* buf, uint64_t value)
{
    buf[0] = (value >> 56) & 0xFF;
    buf[1] = (value >> 48) & 0xFF;
    buf[2] = (value >> 40) & 0xFF;
    buf[3] = (value >> 32) & 0xFF;
    buf[4] = (value >> 24) & 0xFF;
    buf[5] = (value >> 16) & 0xFF;
    buf[6] = (value >> 8) & 0xFF;
    buf[7] = value & 0xFF;
}

// Helper: Encode variable-length payload size
static size_t encode_payload_size(uint8_t* buf, size_t payload_size)
{
    size_t written = 0;
    while (payload_size >= 255)
    {
        buf[written++] = 0xFF;
        payload_size -= 255;
    }
    buf[written++] = (uint8_t)payload_size;
    return written;
}

int SEI_GenerateTimestampNAL(
    int codec,
    uint64_t timestamp_ns,
    uint64_t frame_index,
    uint8_t* out_buffer,
    size_t* out_size)
{
    if (!out_buffer || !out_size)
        return -1;

    uint8_t* p = out_buffer;
    size_t offset = 0;

    // NAL unit start code: 0x00 0x00 0x00 0x01
    p[offset++] = 0x00;
    p[offset++] = 0x00;
    p[offset++] = 0x00;
    p[offset++] = 0x01;

    // NAL unit header
    if (codec == 1) // HEVC
    {
        // NAL type = 39 (PREFIX_SEI_NUT), layer_id = 0, temporal_id = 1
        p[offset++] = (39 << 1); // (nal_type << 1)
        p[offset++] = 1;         // (layer_id << 3) | temporal_id_plus1
    }
    else // AVC
    {
        // NAL type = 6 (SEI), ref_idc = 0
        p[offset++] = 0x06;
    }

    // SEI payload type = 5 (user_data_unregistered)
    p[offset++] = 5;

    // SEI payload: UUID (16 bytes) + timestamp (8 bytes) + frame_index (8 bytes) = 32 bytes
    const size_t payload_size = 16 + 8 + 8;

    // Encode payload size
    offset += encode_payload_size(&p[offset], payload_size);

    // Write UUID
    memcpy(&p[offset], SEI_LATENCY_UUID, 16);
    offset += 16;

    // Write timestamp (big-endian)
    write_u64_be(&p[offset], timestamp_ns);
    offset += 8;

    // Write frame index (big-endian)
    write_u64_be(&p[offset], frame_index);
    offset += 8;

    // RBSP trailing bits (0x80)
    p[offset++] = 0x80;

    *out_size = offset;
    return 0;
}
