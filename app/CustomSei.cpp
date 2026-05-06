#include "CustomSei.h"
#include <cstring>

/*
 * SEI user_data_unregistered payload format used in this module:
 * 1) Start code: 0x00 0x00 0x00 0x01
 * 2) NAL header: AVC uses 1 byte (type=6), HEVC uses 2 bytes (type=39, PREFIX_SEI_NUT)
 * 3) payloadType: 5 (user_data_unregistered)
 * 4) payloadSize: variable-length byte encoding (0xFF chunks + remainder)
 * 5) payload data:
 *    - 16 bytes UUID (sei_latency_uuid)
 *    - 8 bytes timestamp_us (big-endian)
 *    - 8 bytes reserved_dat (big-endian)
 * 6) RBSP trailing bits: 0x80
 */

static void write_u64_be(uint8_t *buf, uint64_t value)
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

static std::size_t encode_payload_size(uint8_t *buf, std::size_t payload_size)
{
    std::size_t written = 0;
    while (payload_size >= 255)
    {
        buf[written++] = 0xFF;
        payload_size -= 255;
    }
    buf[written++] = (uint8_t)payload_size;
    return written;
}

static uint64_t read_u64_be(const uint8_t *buf)
{
    return (static_cast<uint64_t>(buf[0]) << 56) | (static_cast<uint64_t>(buf[1]) << 48) |
           (static_cast<uint64_t>(buf[2]) << 40) | (static_cast<uint64_t>(buf[3]) << 32) |
           (static_cast<uint64_t>(buf[4]) << 24) | (static_cast<uint64_t>(buf[5]) << 16) |
           (static_cast<uint64_t>(buf[6]) << 8) | static_cast<uint64_t>(buf[7]);
}

int sei_generate_timestamp_nal(bool is_hevc, uint64_t timestamp_us, uint64_t reserved, uint8_t *out_buffer,
                               std::size_t *out_size)
{
    if (!out_buffer || !out_size)
    {
        return -1;
    }

    uint8_t *p = out_buffer;
    std::size_t offset = 0;

    p[offset++] = 0x00;
    p[offset++] = 0x00;
    p[offset++] = 0x00;
    p[offset++] = 0x01;

    if (is_hevc)
    {
        p[offset++] = (39 << 1); // (nal_type << 1)
        p[offset++] = 1;         // (layer_id << 3) | temporal_id_plus1
    }
    else // AVC
    {
        p[offset++] = 0x06;
    }

    p[offset++] = 5;
    const std::size_t payload_size = 16 + 8 + 8;
    offset += encode_payload_size(&p[offset], payload_size);
    std::memcpy(&p[offset], sei_latency_uuid, 16);
    offset += 16;
    write_u64_be(&p[offset], timestamp_us);
    offset += 8;
    write_u64_be(&p[offset], reserved);
    offset += 8;
    p[offset++] = 0x80;
    *out_size = offset;
    return 0;
}

bool parse_sei_timestamp(const uint8_t *payload, std::size_t payload_size, uint64_t &timestamp_us, uint64_t &reserved)
{
    // Payload layout: 16-byte UUID | 8-byte timestamp_us (BE) | 8-byte reserved (BE)
    if (!payload || payload_size < 32)
    {
        return false;
    }

    if (std::memcmp(payload, sei_latency_uuid, 16) != 0)
    {
        return false;
    }

    timestamp_us = read_u64_be(payload + 16);
    reserved = read_u64_be(payload + 24);
    return true;
}