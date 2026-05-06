#ifndef SEI_PARSER_H
#define SEI_PARSER_H

#include "SEITimestamp.h"
#include <cstdint>
#include <cstring>


class SEIParser
{
  public:
    /**
     * @brief Parse bitstream chunk for SEI timestamp
     *
     * @param data Bitstream data
     * @param size Data size in bytes
     * @param timestamp_ns Output: extracted timestamp in nanoseconds
     * @param frame_index Output: extracted frame index
     * @return true if SEI timestamp found and parsed successfully
     */
    static bool parse_sei_timestamp(const uint8_t *data, size_t size, uint64_t &timestamp_ns, uint64_t &frame_index)
    {
        if (!data || size < 32)
            return false;

        // Search for start code followed by SEI NAL
        for (size_t i = 0; i < size - 32; ++i)
        {
            // Look for start code: 0x00 0x00 0x00 0x01
            if (data[i] != 0x00 || data[i + 1] != 0x00 || data[i + 2] != 0x00 || data[i + 3] != 0x01)
            {
                continue;
            }

            size_t pos = i + 4;

            // Check NAL type
            uint8_t nal_header = data[pos];

            bool is_sei = false;
            if ((nal_header & 0x1F) == 6) // AVC SEI
            {
                is_sei = true;
                pos += 1;
            }
            else if ((nal_header >> 1) == 39) // HEVC PREFIX_SEI
            {
                is_sei = true;
                pos += 2; // 2-byte HEVC NAL header
            }

            if (!is_sei)
                continue;

            // Check SEI payload type (should be 5 = user_data_unregistered)
            if (pos >= size || data[pos] != 5)
                continue;
            pos++;

            // Skip payload size encoding
            while (pos < size && data[pos] == 0xFF)
                pos++;
            if (pos >= size)
                continue;
            pos++; // Skip last size byte

            // Check UUID
            if (pos + 32 > size)
                continue;

            if (std::memcmp(&data[pos], SEI_LATENCY_UUID, 16) != 0)
                continue;
            pos += 16;

            // Extract timestamp (big-endian)
            timestamp_ns = read_u64_be(&data[pos]);
            pos += 8;

            // Extract frame index (big-endian)
            frame_index = read_u64_be(&data[pos]);

            return true;
        }

        return false;
    }

  private:
    static uint64_t read_u64_be(const uint8_t *buf)
    {
        return (static_cast<uint64_t>(buf[0]) << 56) | (static_cast<uint64_t>(buf[1]) << 48) |
               (static_cast<uint64_t>(buf[2]) << 40) | (static_cast<uint64_t>(buf[3]) << 32) |
               (static_cast<uint64_t>(buf[4]) << 24) | (static_cast<uint64_t>(buf[5]) << 16) |
               (static_cast<uint64_t>(buf[6]) << 8) | static_cast<uint64_t>(buf[7]);
    }
};

#endif // SEI_PARSER_H
