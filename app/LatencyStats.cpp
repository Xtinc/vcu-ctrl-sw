#include "LatencyStats.h"
#include "ClockSync.h"
#include "SeiParser.h"

extern "C"
{
#include "lib_common/BufferHandleMeta.h"
#include "lib_common/BufferSeiMeta.h"
#include "lib_decode/lib_decode.h"
#include "lib_rtos/message.h"
}

#include <chrono>

LatencyMeasurer::LatencyMeasurer(uint32_t log_interval) : m_log_interval(log_interval), m_frame_count(0)
{
}

LatencyMeasurer::~LatencyMeasurer() = default;

void LatencyMeasurer::start(const std::string &server_ip, uint16_t port)
{
    m_clock_sync = std::make_unique<ClockSync>();
    m_clock_sync->start_client(server_ip, port);
}

void LatencyMeasurer::on_sei(AL_TBuffer *pParsedFrame, int iParsingId)
{
    AL_THandleMetaData *pHandlesMeta = (AL_THandleMetaData *)AL_Buffer_GetMetaData(pParsedFrame, AL_META_TYPE_HANDLE);
    if (!pHandlesMeta)
    {
        return;
    }

    if (iParsingId > AL_HandleMetaData_GetNumHandles(pHandlesMeta))
    {
        VIDEO_ERROR_PRINT("LatencyMeasurer::on_sei: ParsingId %d is out of bounds (num handles = %d)", iParsingId,
                          AL_HandleMetaData_GetNumHandles(pHandlesMeta));
        return;
    }

    AL_TDecMetaHandle *pDecMetaHandle = (AL_TDecMetaHandle *)AL_HandleMetaData_GetHandle(pHandlesMeta, iParsingId);
    if (pDecMetaHandle->eState == AL_DEC_HANDLE_STATE_PROCESSED)
    {
        AL_TBuffer *pStream = pDecMetaHandle->pHandle;
        if (!pStream)
        {
            VIDEO_ERROR_PRINT("LatencyMeasurer::on_sei: pStream is not allocated in handle for parsingId %d",
                              iParsingId);
            return;
        }

        auto seiMeta = (AL_TSeiMetaData *)AL_Buffer_GetMetaData(pStream, AL_META_TYPE_SEI);
        if (!seiMeta)
        {
            return;
        }
        AL_Buffer_RemoveMetaData(pStream, (AL_TMetaData *)seiMeta);
        FrameSeiData data{};
        SEIParser::parse_sei_timestamp(seiMeta->payload->pData, seiMeta->payload->size, data.timestamp_us,
                                       data.frame_index);
        {
            std::lock_guard<std::mutex> lock(m_sei_map_mutex);
            m_frame_sei_map[pParsedFrame] = data;
        }
    }
}

void LatencyMeasurer::on_frame_displayed(AL_TBuffer *pDisplayedFrame)
{
    if (!m_clock_sync || !m_clock_sync->is_synchronized())
    {
        return;
    }

    const auto now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    uint64_t sei_ts_us = 0;
    uint64_t frame_index = 0;
    {
        std::lock_guard<std::mutex> lock(m_sei_map_mutex);
        auto it = m_frame_sei_map.find(pDisplayedFrame);
        if (it != m_frame_sei_map.end())
        {
            sei_ts_us = it->second.timestamp_us;
            frame_index = it->second.frame_index;
            m_frame_sei_map.erase(it);
        }
    }
    const double latency_ms = (now_us - static_cast<int64_t>(sei_ts_us) - m_clock_sync->get_offset_us());

    if (latency_ms <= 0 || latency_ms >= 10000)
        return;

    m_stats.add(latency_ms);

    if (++m_frame_count % m_log_interval == 0)
    {
        VIDEO_INFO_PRINT("[Latency] frame=%llu p50=%.2fms p95=%.2fms p99=%.2fms",
                         static_cast<unsigned long long>(frame_index), m_stats.quantile(0.5), m_stats.quantile(0.95),
                         m_stats.quantile(0.99));
    }
}

LatencyInjector::LatencyInjector() : m_latency_start_time(std::chrono::steady_clock::now()), m_frame_index(0)
{
    m_sei_buffer.resize(16 * 1024);
}

LatencyInjector::~LatencyInjector()
{
}

void LatencyInjector::start(const std::string &server_ip, uint16_t port)
{
    m_clock_sync = std::make_unique<ClockSync>();
    m_clock_sync->start_server(port);
}

void LatencyInjector::on_submitted_frame()
{
    auto now = std::chrono::steady_clock::now();
    auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();

    std::lock_guard<std::mutex> lock(m_timestamp_mutex);
    m_frame_timestamps[m_frame_index++] = static_cast<uint64_t>(timestamp_us);
}

std::pair<const uint8_t *, size_t> LatencyInjector::inject_sei(AL_EProfile profile, const uint8_t *encoded_data,
                                                               size_t encoded_size)
{
    uint64_t timestamp_ns = 0;
    uint64_t frame_idx = 0;
    {
        std::lock_guard<std::mutex> lock(m_timestamp_mutex);
        if (!m_frame_timestamps.empty())
        {
            auto it = m_frame_timestamps.begin();
            frame_idx = it->first;
            timestamp_ns = it->second;
            m_frame_timestamps.erase(it);
        }
    }

    if (timestamp_ns == 0)
    {
        return {encoded_data, encoded_size};
    }

    // Generate SEI NAL unit
    uint8_t sei_nal[SEI_TIMESTAMP_MAX_SIZE];
    size_t sei_size = 0;
    int codec = (profile == AL_PROFILE_HEVC_MAIN || profile == AL_PROFILE_HEVC_MAIN10) ? SEI_CODEC_HEVC : SEI_CODEC_AVC;

    if (SEIParser::SEI_GenerateTimestampNAL(codec, timestamp_ns, frame_idx, sei_nal, &sei_size) != 0 || sei_size == 0)
    {
        return {encoded_data, encoded_size};
    }

    // Prepend SEI to encoded data
    std::memcpy(m_sei_buffer.data(), sei_nal, sei_size);
    std::memcpy(m_sei_buffer.data() + sei_size, encoded_data, encoded_size);

    return {m_sei_buffer.data(), m_sei_buffer.size()};
}
