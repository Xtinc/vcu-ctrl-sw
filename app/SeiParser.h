#ifndef SEI_PARSER_H
#define SEI_PARSER_H

#include <cstddef>
#include <cstdint>

struct SEI_TimestampPayload
{
    uint8_t uuid[16];
    uint64_t timestamp_ns;
    uint64_t frame_index;
};

constexpr int SEI_CODEC_AVC = 0;
constexpr int SEI_CODEC_HEVC = 1;
constexpr std::size_t SEI_TIMESTAMP_MAX_SIZE = 64;
constexpr uint8_t SEI_LATENCY_UUID[16] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x47, 0x89,
                                          0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x78, 0x90};

/**
 * @brief Utility class for parsing SEI timestamp information from bitstream data.
 */
class SEIParser
{
  public:
    /**
     * @brief Generate an SEI user_data_unregistered NAL unit for latency measurement.
     *
     * Creates a complete SEI NAL unit and stores it in @p out_buffer.
     *
     * @param codec Codec type. Use @ref SEI_CODEC_AVC or @ref SEI_CODEC_HEVC.
     * @param timestamp_ns Frame encode timestamp in nanoseconds.
     * @param frame_index Sequential frame index.
     * @param out_buffer Output buffer. Required capacity is at least @ref SEI_TIMESTAMP_MAX_SIZE.
     * @param out_size Output size of generated NAL unit in bytes.
     * @return 0 on success, negative value on error.
     */
    static int SEI_GenerateTimestampNAL(int codec, uint64_t timestamp_ns, uint64_t frame_index, uint8_t *out_buffer,
                                        std::size_t *out_size);

    /**
     * @brief Parse bitstream chunk and extract timestamp SEI payload.
     *
     * @param data Pointer to bitstream data.
     * @param size Bitstream data size in bytes.
     * @param timestamp_ns Output timestamp in nanoseconds.
     * @param frame_index Output frame index.
     * @return true if a valid timestamp SEI payload is found, otherwise false.
     */
    static bool parse_sei_timestamp(const uint8_t *data, std::size_t size, uint64_t &timestamp_ns,
                                    uint64_t &frame_index);

  private:
    static uint64_t read_u64_be(const uint8_t *buf);
};

#endif // SEI_PARSER_H
