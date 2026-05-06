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

LatencyInjector::LatencyInjector()
{
    m_buffer.resize(16 * 1024);
}

void LatencyInjector::start(const std::string &server_ip, uint16_t port)
{
    m_clock = std::make_unique<ClockSync>();
    m_clock->start_server(port);
}

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

uint64_t LatencyInjector::pop_frame_timestamp(AL_TBuffer *pSrcFrame)
{
    if (!pSrcFrame)
    {
        return 0;
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_frames.find(pSrcFrame);
    if (it == m_frames.end())
    {
        return 0;
    }

    const uint64_t timestamp_us = it->second.timestamp;
    m_frames.erase(it);
    return timestamp_us;
}

std::pair<const uint8_t *, size_t> LatencyInjector::on_frame_encoded(int profile, const uint8_t *encoded_data,
                                                                     size_t encoded_size, AL_TBuffer *pSrcFrame)
{
    const uint64_t timestamp_us = pop_frame_timestamp(pSrcFrame);

    if (timestamp_us == 0)
    {
        return {encoded_data, encoded_size};
    }

    // Generate SEI NAL unit
    uint8_t sei_nal[sei_timestamp_max_size];
    size_t sei_size = 0;
    uint64_t frame_idx = 0;
    const bool is_hevc = (profile == AL_PROFILE_HEVC_MAIN || profile == AL_PROFILE_HEVC_MAIN10);

    if (sei_generate_timestamp_nal(is_hevc, timestamp_us, frame_idx, sei_nal, &sei_size) != 0 || sei_size == 0)
    {
        return {encoded_data, encoded_size};
    }

    // Prepend SEI to encoded data
    std::memcpy(m_buffer.data(), sei_nal, sei_size);
    std::memcpy(m_buffer.data() + sei_size, encoded_data, encoded_size);

    return {m_buffer.data(), sei_size + encoded_size};
}

LatencyMeasurer::LatencyMeasurer(uint32_t log_interval) : m_log_interval(log_interval), m_frame_count(0)
{
}

void LatencyMeasurer::start(const std::string &server_ip, uint16_t port)
{
    m_clock_sync = std::make_unique<ClockSync>();
    m_clock_sync->start_client(server_ip, port);
}

bool LatencyMeasurer::try_extract_sei_data(AL_TBuffer *pParsedFrame, int iParsingId, FrameSeiData &out_data) const
{
    AL_THandleMetaData *pHandlesMeta = (AL_THandleMetaData *)AL_Buffer_GetMetaData(pParsedFrame, AL_META_TYPE_HANDLE);
    if (!pHandlesMeta)
    {
        return false;
    }

    if (iParsingId > AL_HandleMetaData_GetNumHandles(pHandlesMeta))
    {
        VIDEO_ERROR_PRINT("LatencyMeasurer::on_sei: ParsingId %d is out of bounds (num handles = %d)", iParsingId,
                          AL_HandleMetaData_GetNumHandles(pHandlesMeta));
        return false;
    }

    AL_TDecMetaHandle *pDecMetaHandle = (AL_TDecMetaHandle *)AL_HandleMetaData_GetHandle(pHandlesMeta, iParsingId);
    if (pDecMetaHandle->eState != AL_DEC_HANDLE_STATE_PROCESSED)
    {
        return false;
    }

    AL_TBuffer *pStream = pDecMetaHandle->pHandle;
    if (!pStream)
    {
        VIDEO_ERROR_PRINT("LatencyMeasurer::on_sei: pStream is not allocated in handle for parsingId %d", iParsingId);
        return false;
    }

    auto seiMeta = (AL_TSeiMetaData *)AL_Buffer_GetMetaData(pStream, AL_META_TYPE_SEI);
    if (!seiMeta)
    {
        return false;
    }

    AL_Buffer_RemoveMetaData(pStream, (AL_TMetaData *)seiMeta);
    parse_sei_timestamp(seiMeta->payload->pData, seiMeta->payload->size, out_data.timestamp, out_data.reserved);
    return true;
}

void LatencyMeasurer::on_sei(AL_TBuffer *pParsedFrame, int iParsingId)
{
    FrameSeiData data{};
    std::lock_guard<std::mutex> lock(m_sei_map_mutex);
    if (!try_extract_sei_data(pParsedFrame, iParsingId, data))
    {
        return;
    }
    m_frame_sei_map[pParsedFrame] = data;
}

bool LatencyMeasurer::take_frame_sei_data(AL_TBuffer *pDisplayedFrame, FrameSeiData &out_data)
{
    auto it = m_frame_sei_map.find(pDisplayedFrame);
    if (it == m_frame_sei_map.end())
    {
        return false;
    }

    out_data = it->second;
    m_frame_sei_map.erase(it);
    return true;
}

void LatencyMeasurer::log_stats_if_needed(uint64_t frame_index)
{
    if (++m_frame_count % m_log_interval != 0)
    {
        return;
    }

    VIDEO_INFO_PRINT("[Latency] frame=%llu p50=%.2fms p95=%.2fms p99=%.2fms",
                     static_cast<unsigned long long>(frame_index), m_stats.quantile(0.5), m_stats.quantile(0.95),
                     m_stats.quantile(0.99));
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
    FrameSeiData frame_data{};
    {
        std::lock_guard<std::mutex> lock(m_sei_map_mutex);
        if (!take_frame_sei_data(pDisplayedFrame, frame_data))
        {
            return;
        }
    }

    const double latency_ms = (now_us - static_cast<int64_t>(frame_data.timestamp) - m_clock_sync->get_offset_us());

    if (latency_ms <= 0 || latency_ms >= 10000)
    {
        return;
    }

    m_stats.add(latency_ms);
    log_stats_if_needed(frame_data.reserved);
}
