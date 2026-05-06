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