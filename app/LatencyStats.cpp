#include "LatencyStats.h"

#include <cstring>

extern "C"
{
#include "lib_common/BufferStreamMeta.h"
#include "lib_encode/lib_encoder.h"
#include "lib_rtos/message.h"
}

static constexpr std::size_t kLatencySeiPayloadSize = 32;
static constexpr int kSeiPayloadTypeUserDataUnregistered = 5;
static constexpr uint8_t kLatencySeiUuid[16] = {0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x47, 0x89,
                                                0xa1, 0xb2, 0xc3, 0xd4, 0xe5, 0xf6, 0x78, 0x90};

static uint64_t read_u64_be(const uint8_t *buf)
{
    return (static_cast<uint64_t>(buf[0]) << 56) | (static_cast<uint64_t>(buf[1]) << 48) |
           (static_cast<uint64_t>(buf[2]) << 40) | (static_cast<uint64_t>(buf[3]) << 32) |
           (static_cast<uint64_t>(buf[4]) << 24) | (static_cast<uint64_t>(buf[5]) << 16) |
           (static_cast<uint64_t>(buf[6]) << 8) | static_cast<uint64_t>(buf[7]);
}

static void write_u64_be(uint8_t *buf, uint64_t value)
{
    buf[0] = static_cast<uint8_t>((value >> 56) & 0xFF);
    buf[1] = static_cast<uint8_t>((value >> 48) & 0xFF);
    buf[2] = static_cast<uint8_t>((value >> 40) & 0xFF);
    buf[3] = static_cast<uint8_t>((value >> 32) & 0xFF);
    buf[4] = static_cast<uint8_t>((value >> 24) & 0xFF);
    buf[5] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buf[6] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buf[7] = static_cast<uint8_t>(value & 0xFF);
}

static void build_latency_sei_payload(uint8_t *payload, const FrameSeiData &data)
{
    std::memset(payload, 0, kLatencySeiPayloadSize);
    std::memcpy(payload, kLatencySeiUuid, sizeof(kLatencySeiUuid));
    write_u64_be(payload + 16, data.timestamp);
    write_u64_be(payload + 24, data.reserved);
}

static bool parse_latency_sei_payload(const uint8_t *payload, std::size_t payload_size, FrameSeiData &out_data)
{
    if (!payload || payload_size < kLatencySeiPayloadSize)
    {
        return false;
    }

    if (std::memcmp(payload, kLatencySeiUuid, sizeof(kLatencySeiUuid)) != 0)
    {
        return false;
    }

    out_data.timestamp = read_u64_be(payload + 16);
    out_data.reserved = read_u64_be(payload + 24);
    return true;
}

LatencyInjector::LatencyInjector() = default;

LatencyInjector::~LatencyInjector() = default;

void LatencyInjector::on_frame_submitted(AL_TBuffer *pSrcFrame)
{
    if (!pSrcFrame)
    {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
    std::lock_guard<std::mutex> lock(m_mutex);
    m_frames[pSrcFrame] = FrameSeiData{static_cast<uint64_t>(timestamp_us), 0};
}

bool LatencyInjector::pop_frame_data(AL_TBuffer *pSrcFrame, FrameSeiData &out_data)
{
    if (!pSrcFrame)
    {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_frames.find(pSrcFrame);
    if (it == m_frames.end())
    {
        return false;
    }

    out_data = it->second;
    m_frames.erase(it);
    return true;
}

bool LatencyInjector::on_frame_encoded(AL_HEncoder hEnc, AL_TBuffer *pStream, AL_TBuffer *pSrcFrame)
{
    FrameSeiData data{};
    if (!pop_frame_data(pSrcFrame, data))
    {
        return true;
    }

    // user_data_unregistered payload: UUID(16) + timestamp_us(8) + reserved(8)
    uint8_t payload[kLatencySeiPayloadSize] = {};
    build_latency_sei_payload(payload, data);

    auto *stream_meta = reinterpret_cast<AL_TStreamMetaData *>(AL_Buffer_GetMetaData(pStream, AL_META_TYPE_STREAM));
    if (!stream_meta)
    {
        VIDEO_ERROR_PRINT("LatencyInjector: missing stream metadata for SEI injection");
        return false;
    }

    const int section_id = AL_Encoder_AddSei(hEnc, pStream, true, kSeiPayloadTypeUserDataUnregistered, payload,
                                             sizeof(payload), stream_meta->uTemporalID);
    if (section_id < 0)
    {
        VIDEO_ERROR_PRINT("LatencyInjector: AL_Encoder_AddSei failed (section_id=%d)", section_id);
        return false;
    }

    return true;
}

void LatencyInjector::on_frame_skipped(AL_TBuffer *pSrcFrame)
{
    FrameSeiData dummy{};
    (void)pop_frame_data(pSrcFrame, dummy);
}

LatencyMeasurer::LatencyMeasurer() = default;

LatencyMeasurer::~LatencyMeasurer() = default;

void LatencyMeasurer::on_parsed_sei(bool /*is_prefix*/, int payload_type, uint8_t *payload, int payload_size)
{
    if (payload_type != kSeiPayloadTypeUserDataUnregistered)
    {
        return;
    }

    FrameSeiData data{};
    if (!parse_latency_sei_payload(payload, static_cast<std::size_t>(payload_size), data))
    {
        return;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_parsed_sei_fifo.push_back(data);
}

bool LatencyMeasurer::take_parsed_sei_data(FrameSeiData &out_data)
{
    if (m_parsed_sei_fifo.empty())
    {
        return false;
    }

    out_data = m_parsed_sei_fifo.front();
    m_parsed_sei_fifo.pop_front();
    return true;
}

void LatencyMeasurer::on_frame_displayed(AL_TBuffer * /*pDisplayedFrame*/)
{
    const auto now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    FrameSeiData frame_data{};

    std::lock_guard<std::mutex> lock(m_mutex);
    if (!take_parsed_sei_data(frame_data))
    {
        return;
    }

    const double latency_ms = (now_us - static_cast<double>(frame_data.timestamp)) / 1000.0;
    m_stats.add(latency_ms);
}
