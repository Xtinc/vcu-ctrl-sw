#include "SeiParser.h"

#include <cstddef>
#include <cstring>

/*
 * SEI user_data_unregistered payload format used in this module:
 * 1) Start code: 0x00 0x00 0x00 0x01
 * 2) NAL header: AVC uses 1 byte (type=6), HEVC uses 2 bytes (type=39, PREFIX_SEI_NUT)
 * 3) payloadType: 5 (user_data_unregistered)
 * 4) payloadSize: variable-length byte encoding (0xFF chunks + remainder)
 * 5) payload data:
 *    - 16 bytes UUID (SEI_LATENCY_UUID)
 *    - 8 bytes timestamp_ns (big-endian)
 *    - 8 bytes frame_index (big-endian)
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

int SEIParser::SEI_GenerateTimestampNAL(int codec, uint64_t timestamp_ns, uint64_t frame_index, uint8_t *out_buffer,
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

    if (codec == SEI_CODEC_HEVC)
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
    std::memcpy(&p[offset], SEI_LATENCY_UUID, 16);
    offset += 16;
    write_u64_be(&p[offset], timestamp_ns);
    offset += 8;
    write_u64_be(&p[offset], frame_index);
    offset += 8;
    p[offset++] = 0x80;
    *out_size = offset;
    return 0;
}

bool SEIParser::parse_sei_timestamp(const uint8_t *data, std::size_t size, uint64_t &timestamp_ns,
                                    uint64_t &frame_index)
{
    if (!data || size < 32)
    {
        return false;
    }

    for (std::size_t i = 0; i < size - 32; ++i)
    {
        if (data[i] != 0x00 || data[i + 1] != 0x00 || data[i + 2] != 0x00 || data[i + 3] != 0x01)
        {
            continue;
        }

        std::size_t pos = i + 4;
        const uint8_t nal_header = data[pos];

        bool is_sei = false;
        if ((nal_header & 0x1F) == 6)
        {
            is_sei = true;
            pos += 1;
        }
        else if ((nal_header >> 1) == 39)
        {
            is_sei = true;
            pos += 2;
        }

        if (!is_sei)
        {
            continue;
        }

        if (pos >= size || data[pos] != 5)
        {
            continue;
        }
        ++pos;

        while (pos < size && data[pos] == 0xFF)
        {
            ++pos;
        }
        if (pos >= size)
        {
            continue;
        }
        ++pos;

        if (pos + 32 > size)
        {
            continue;
        }

        if (std::memcmp(&data[pos], SEI_LATENCY_UUID, 16) != 0)
        {
            continue;
        }
        pos += 16;

        timestamp_ns = read_u64_be(&data[pos]);
        pos += 8;
        frame_index = read_u64_be(&data[pos]);
        return true;
    }

    return false;
}

uint64_t SEIParser::read_u64_be(const uint8_t *buf)
{
    return (static_cast<uint64_t>(buf[0]) << 56) | (static_cast<uint64_t>(buf[1]) << 48) |
           (static_cast<uint64_t>(buf[2]) << 40) | (static_cast<uint64_t>(buf[3]) << 32) |
           (static_cast<uint64_t>(buf[4]) << 24) | (static_cast<uint64_t>(buf[5]) << 16) |
           (static_cast<uint64_t>(buf[6]) << 8) | static_cast<uint64_t>(buf[7]);
}