#ifndef CUSTOM_SEI_H
#define CUSTOM_SEI_H

#include <cstddef>
#include <cstdint>

constexpr std::size_t sei_timestamp_max_size = 64;
constexpr uint8_t sei_latency_uuid[16] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x47, 0x89,
                                          0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x78, 0x90};

/**
 * @brief Generate an SEI user_data_unregistered NAL unit for latency measurement.
 *
 * Creates a complete SEI NAL unit and stores it in @p out_buffer.
 *
 * @param is_hevc true for HEVC, false for AVC.
 * @param timestamp_us Frame encode timestamp.
 * @param reserved reserved data.
 * @param out_buffer Output buffer. Required capacity is at least @ref sei_timestamp_max_size.
 * @param out_size Output size of generated NAL unit in bytes.
 * @return 0 on success, negative value on error.
 */
int sei_generate_timestamp_nal(bool is_hevc, uint64_t timestamp_us, uint64_t reserved, uint8_t *out_buffer,
                               std::size_t *out_size);

/**
 * @brief Parse an SEI user_data_unregistered payload and extract the timestamp.
 *
 * Expects the raw payload bytes as provided by the decoder's parsedSeiCB
 * (anti-emulation already removed). The payload must begin with
 * @ref sei_latency_uuid followed by an 8-byte big-endian timestamp and an
 * 8-byte big-endian frame index.
 *
 * @param payload     Pointer to the SEI payload (UUID + timestamp + frame_index).
 * @param payload_size Size of the payload in bytes. Must be >= 32.
 * @param timestamp_us Output timestamp.
 * @param reserved  reserved data.
 * @return true if the payload contains a valid latency timestamp, otherwise false.
 */
bool parse_sei_timestamp(const uint8_t *payload, std::size_t payload_size, uint64_t &timestamp_us, uint64_t &reserved);

#endif // CUSTOM_SEI_H
