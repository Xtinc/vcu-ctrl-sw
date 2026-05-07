#include "LatencyStats.h"
#include "ClockSync.h"
#include "CustomSei.h"

extern "C"
{
#include "lib_common/BufferHandleMeta.h"
#include "lib_common/BufferSeiMeta.h"
#include "lib_common/BufferStreamMeta.h"
#include "lib_decode/lib_decode.h"
#include "lib_encode/lib_encoder.h"
#include "lib_rtos/message.h"
}

LatencyInjector::LatencyInjector()
{
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
    uint8_t payload[32] = {};
    std::memcpy(payload, sei_latency_uuid, 16);
    for (int i = 0; i < 8; ++i)
    {
        payload[16 + i] = static_cast<uint8_t>((data.timestamp >> ((7 - i) * 8)) & 0xFF);
        payload[24 + i] = static_cast<uint8_t>((data.reserved >> ((7 - i) * 8)) & 0xFF);
    }

    auto *stream_meta = (AL_TStreamMetaData *)AL_Buffer_GetMetaData(pStream, AL_META_TYPE_STREAM);
    if (!stream_meta)
    {
        VIDEO_ERROR_PRINT("LatencyInjector: missing stream metadata for SEI injection");
        return false;
    }

    constexpr int sei_payload_type_user_data_unregistered = 5;
    const int section_id = AL_Encoder_AddSei(hEnc, pStream, true, sei_payload_type_user_data_unregistered, payload,
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
