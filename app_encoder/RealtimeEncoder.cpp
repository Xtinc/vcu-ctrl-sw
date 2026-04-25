// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "RealtimeEncoder.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_common/BufferPixMapMeta.h"
#include "lib_common/BufferStreamMeta.h"
#include "lib_common/HardwareDriver.h"
#include "lib_common/PixMapBuffer.h"
#include "lib_common/Planes.h"
#include "lib_common/RoundUp.h"
#include "lib_common/StreamBuffer.h"
#include "lib_common_enc/EncBuffers.h"
#include "lib_common_enc/IpEncFourCC.h"
#include "lib_encode/EncSchedulerMcu.h"
#include "lib_fpga/DmaAlloc.h"
#include "lib_fpga/DmaAllocLinux.h"
#include "lib_rtos/message.h"
}

static constexpr int kStreamSmoothingCount = 2;
static constexpr int kHeightAlign = 8;

static uint32_t WriteOneSection(DMAProxy &mem_ctl, AL_TBuffer *source, AL_TBuffer *destination, int offset,
                                int numSection)
{
    auto meta = reinterpret_cast<AL_TStreamMetaData *>(AL_Buffer_GetMetaData(source, AL_META_TYPE_STREAM));
    auto &section = meta->pSections[numSection];

    if (section.uLength == 0 || (section.eFlags & AL_SECTION_END_FRAME_FLAG))
    {
        return 0;
    }

    DBG_ASSERT_GE(source->zSizes[0], section.uOffset);

    auto size = source->zSizes[0] - section.uOffset;
    if (size < section.uLength)
    {
        mem_ctl.move(destination, offset, source, section.uOffset, size);
        mem_ctl.move(destination, offset, source, 0, section.uLength - size);
    }
    else
    {
        mem_ctl.move(destination, offset, source, section.uOffset, section.uLength);
    }

    return section.uLength;
}

static uint32_t WriteFillerDataSection(DMAProxy &mem_ctl, AL_TBuffer *source, AL_TBuffer *destination, int offset,
                                       int numSection)
{
    auto meta = reinterpret_cast<AL_TStreamMetaData *>(AL_Buffer_GetMetaData(source, AL_META_TYPE_STREAM));
    auto &section = meta->pSections[numSection];

    auto src = AL_Buffer_GetData(source);
    auto dst = AL_Buffer_GetData(destination);
    auto srcOffset = section.uOffset;
    auto dstOffset = offset;
    auto length = section.uLength;

    while (--length && (src[srcOffset] != 0xFF))
    {
        dst[dstOffset++] = src[srcOffset++];
    }

    if (length > 0)
    {
        mem_ctl.set(destination, dstOffset, 0xFF, length);
    }

    DBG_ASSERT_COND(src[srcOffset + length] == 0x80);
    dst[dstOffset + length] = src[srcOffset + length];
    return section.uLength;
}

static uint32_t ReconstructStream(DMAProxy &mem_ctl, AL_TBuffer *stream, int firstSection)
{
    uint32_t size = 0;
    auto meta = (AL_TStreamMetaData *)(AL_Buffer_GetMetaData(stream, AL_META_TYPE_STREAM));

    DBG_ASSERT_COND(meta != nullptr);
    DBG_ASSERT_LE(firstSection, meta->uNumSection);

    for (int i = firstSection; i < meta->uNumSection; i++)
    {
        if (meta->pSections[i].eFlags & AL_SECTION_APP_FILLER_FLAG)
        {
            size += WriteFillerDataSection(mem_ctl, stream, stream, static_cast<int>(size), i);
        }
        else
        {
            size += WriteOneSection(mem_ctl, stream, stream, static_cast<int>(size), i);
        }
    }

    return size;
}

RealtimeEncoder::RealtimeEncoder(const EncoderConfig &cfg, EncodedFrameCallback cb, RealtimeEncoder::WorkMode mode)
    : m_work_mode(mode), m_cfg(cfg), m_callback(std::move(cb)), m_dmaProxy(cfg.sDMAProxyPath.c_str())
{
    if (!m_callback)
    {
        throw std::invalid_argument("RealtimeEncoder: callback must not be null");
    }

    AL_ERR err = AL_Lib_Encoder_Init(AL_LIB_ENCODER_ARCH_HOST);
    if (err != AL_SUCCESS)
    {
        throw std::runtime_error(std::string("AL_Lib_Encoder_Init failed: ") + AL_Codec_ErrorToString(err));
    }

    m_pAllocator = AL_DmaAlloc_Create(m_cfg.sDevicePath.c_str());
    if (!m_pAllocator)
    {
        AL_Lib_Encoder_DeInit();
        throw std::runtime_error("Failed to create DMA allocator for device: " + m_cfg.sDevicePath);
    }

    m_pScheduler = AL_SchedulerMcu_Create(
        AL_GetHardwareDriver(), reinterpret_cast<AL_TLinuxDmaAllocator *>(m_pAllocator), m_cfg.sDevicePath.c_str());
    if (!m_pScheduler)
    {
        AL_Allocator_Destroy(m_pAllocator);
        m_pAllocator = nullptr;
        AL_Lib_Encoder_DeInit();
        throw std::runtime_error("Failed to create MCU scheduler");
    }

    m_picFormat = AL_EncGetSrcPicFormat(m_cfg.eChromaMode, m_cfg.uBitDepth, AL_SRC_RASTER);
    m_srcFourCC = AL_EncGetSrcFourCC(m_picFormat);
    m_pitchY = AL_EncGetMinPitch(m_cfg.width, &m_picFormat);
    m_strideH = AL_RoundUp(static_cast<int>(m_cfg.height), kHeightAlign);

    AL_TEncSettings settings{};
    AL_Settings_SetDefaults(&settings);
    AL_Settings_SetDefaultParam(&settings);
    initSettings(settings);

    auto result = AL_Settings_CheckValidity(&settings, &settings.tChParam[0], nullptr);
    if (result > 0)
    {
        throw std::runtime_error("AL_Settings_CheckValidity found " + std::to_string(result) + " invalid parameter(s)");
    }

    result = AL_Settings_CheckCoherency(&settings, &settings.tChParam[0], m_srcFourCC, nullptr);
    if (result < 0)
    {
        throw std::runtime_error("AL_Settings_CheckCoherency: fatal incoherency");
    }

    err = AL_Encoder_Create(&m_hEnc, m_pScheduler, m_pAllocator, &settings, {&RealtimeEncoder::sdkCallback, this});
    if (err != AL_SUCCESS || m_hEnc == nullptr)
    {
        AL_IEncScheduler_Destroy(m_pScheduler);
        m_pScheduler = nullptr;
        AL_Allocator_Destroy(m_pAllocator);
        m_pAllocator = nullptr;
        AL_Lib_Encoder_DeInit();
        throw std::runtime_error(std::string("AL_Encoder_Create failed: ") + AL_Codec_ErrorToString(err));
    }

    if (!AL_Encoder_SetAutoQP(m_hEnc, true))
    {
        VIDEO_ERROR_PRINT("AL_Encoder_SetAutoQP failed: %s", AL_Codec_ErrorToString(AL_Encoder_GetLastError(m_hEnc)));
    }

    m_srcBufPool = std::make_unique<PixMapBufPool>();
    m_streamBufPool = std::make_unique<BufPool>();
    initSrcBufPool();
    initStreamBufPool();

    pushStreamBuffers();
}

RealtimeEncoder::~RealtimeEncoder()
{
    if (!m_stopped.load())
    {
        try
        {
            flush();
        }
        catch (...)
        {
        }
    }

    if (m_hEnc != nullptr)
    {
        AL_Encoder_Destroy(m_hEnc);
        m_hEnc = nullptr;
    }

    if (m_srcBufPool)
    {
        m_srcBufPool->Decommit();
        m_srcBufPool.reset();
    }
    if (m_streamBufPool)
    {
        m_streamBufPool->Decommit();
        m_streamBufPool.reset();
    }

    if (m_pScheduler)
    {
        AL_IEncScheduler_Destroy(m_pScheduler);
        m_pScheduler = nullptr;
    }
    if (m_pAllocator)
    {
        AL_Allocator_Destroy(m_pAllocator);
        m_pAllocator = nullptr;
    }

    AL_Lib_Encoder_DeInit();
}

AL_TBuffer *RealtimeEncoder::acquireSourceBuffer()
{
    if (m_work_mode != WorkMode::FILE)
    {
        return nullptr;
    }

    if (m_stopped.load() || m_hasError.load())
    {
        return nullptr;
    }
    return m_srcBufPool->GetBuffer(AL_BUF_MODE_BLOCK);
}

bool RealtimeEncoder::submitSourceBuffer(AL_TBuffer *pBuf)
{
    if (m_work_mode != WorkMode::FILE)
    {
        return false;
    }

    if (!pBuf)
    {
        return false;
    }

    if (m_stopped.load() || m_hasError.load())
    {
        AL_Buffer_Unref(pBuf);
        return false;
    }

    /* 引用计数说明：
     *   - acquireSourceBuffer 内 GetBuffer 已使 ref=1（用户引用）
     *   - AL_Encoder_Process 内部 Ref → ref=2（SDK 引用）
     *   - 此处 Unref 释放用户引用 → ref=1，缓冲由 SDK 持有
     *   - 编码完毕后经 source-release 回调自动回池 */
    bool const ok = AL_Encoder_Process(m_hEnc, pBuf, nullptr);
    AL_Buffer_Unref(pBuf);

    if (!ok)
    {
        m_hasError.store(true);
        return false;
    }
    return true;
}

bool RealtimeEncoder::submitDmabufFd(int fd, size_t size, uint64_t token)
{
    if (m_work_mode != WorkMode::V4L2)
    {
        return false;
    }

    if (fd < 0 || size == 0)
    {
        return false;
    }

    if (m_stopped.load() || m_hasError.load())
    {
        return false;
    }

    auto *pLinuxAllocator = reinterpret_cast<AL_TLinuxDmaAllocator *>(m_pAllocator);
    AL_HANDLE dmaHandle = AL_LinuxDmaAllocator_ImportFromFd(pLinuxAllocator, fd);
    if (!dmaHandle)
    {
        return false;
    }

    AL_TDimension tDim{static_cast<int32_t>(m_cfg.width), static_cast<int32_t>(m_cfg.height)};
    AL_TBuffer *pSrcBuf = AL_PixMapBuffer_Create(m_pAllocator, AL_Buffer_Destroy, tDim, m_srcFourCC);
    if (!pSrcBuf)
    {
        AL_Allocator_Free(m_pAllocator, dmaHandle);
        return false;
    }

    AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES]{};
    int nPlanes = AL_Plane_GetBufferPixelPlanes(m_picFormat, usedPlanes);

    std::vector<AL_TPlaneDescription> planeDescs;
    planeDescs.reserve(static_cast<size_t>(nPlanes));

    int offset = 0;
    for (int i = 0; i < nPlanes; ++i)
    {
        int pitch = (usedPlanes[i] == AL_PLANE_Y) ? m_pitchY : AL_GetChromaPitch(m_srcFourCC, m_pitchY);
        planeDescs.push_back(AL_TPlaneDescription{usedPlanes[i], offset, pitch});
        offset += static_cast<int>(AL_GetAllocSizeSrc_PixPlane(&m_picFormat, m_pitchY, m_strideH, usedPlanes[i]));
    }

    if (!AL_PixMapBuffer_AddPlanes(pSrcBuf, dmaHandle, size, planeDescs.data(), nPlanes))
    {
        AL_Buffer_Unref(pSrcBuf);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_externalSourcesMutex);
        m_externalSources[pSrcBuf] = ExternalSourceContext{token};
    }

    bool ok = AL_Encoder_Process(m_hEnc, pSrcBuf, nullptr);
    if (!ok)
    {
        {
            std::lock_guard<std::mutex> lock(m_externalSourcesMutex);
            m_externalSources.erase(pSrcBuf);
        }
        AL_Buffer_Unref(pSrcBuf);
        VIDEO_ERROR_PRINT("Failed to submit dmabuf fd %d to encoder", fd);
        m_hasError.store(true);
        return false;
    }

    AL_Buffer_Unref(pSrcBuf);
    return true;
}

void RealtimeEncoder::setSourceReleaseCallback(SourceReleaseCallback cb)
{
    std::lock_guard<std::mutex> lock(m_sourceReleaseCallbackMutex);
    m_sourceReleaseCallback = std::move(cb);
}

void RealtimeEncoder::flush()
{
    if (m_stopped.exchange(true))
    {
        return;
    }

    /* 先让缓冲池拒绝新的阻塞请求，防止内部线程死锁 */
    m_srcBufPool->Decommit();

    /* 发送 EOS 信号：AL_Encoder_Process(hEnc, nullptr, nullptr) */
    bool const eosQueued = AL_Encoder_Process(m_hEnc, nullptr, nullptr);
    if (!eosQueued)
    {
        m_hasError.store(true);
        throw std::runtime_error("flush failed: unable to queue EOS");
    }

    /* 等待回调通知 EOS，避免无限阻塞 */
    std::unique_lock<std::mutex> lock(m_eosMutex);
    bool const gotEos = m_eosCv.wait_for(lock, std::chrono::seconds(10), [this] { return m_eosReceived; });
    if (!gotEos)
    {
        m_hasError.store(true);
        throw std::runtime_error("flush timeout: EOS callback not received");
    }
}

void RealtimeEncoder::requestKeyFrame()
{
    if (m_hEnc != nullptr)
    {
        AL_Encoder_RestartGop(m_hEnc);
    }
}

bool RealtimeEncoder::setBitrate(uint32_t uTargetBitRate, uint32_t uMaxBitRate)
{
    if (m_hEnc == nullptr)
        return false;

    /* SDK 动态码率修改通过 AL_Encoder_SetBitRate（若 SDK 版本支持）
     * 当前接口版本使用 SetMaxPictureSize 实现上限控制；
     * 码率设置需写入 RC 参数并调用 SetDynamicInput（此处保留扩展点）。
     * 实际工程中可替换为 SDK 提供的动态接口。 */
    (void)uTargetBitRate;
    (void)uMaxBitRate;
    /* TODO: 填充具体 SDK 动态码率修改调用 */
    return true;
}

void RealtimeEncoder::sdkCallback(void *pUserParam, AL_TBuffer *pStream, AL_TBuffer const *pSrc, int /*iLayerID*/)
{
    auto *self = static_cast<RealtimeEncoder *>(pUserParam);
    self->onEncodedFrame(pStream, pSrc);
}

void RealtimeEncoder::onEncodedFrame(AL_TBuffer *pStream, AL_TBuffer const *pSrc)
{
    /*
     * SDK 回调四种情形（参见 lib_encoder.h AL_CB_EndEncoding 文档）：
     *
     *  !pStream &&  pSrc  → source-release：SDK 已经完成对源帧的引用释放，
     *                        缓冲引用计数由 SDK 内部管理，到 0 时自动回池；
     *                        用户无需任何操作，直接返回。
     *
     *   pStream && !pSrc  → stream-release：仅通知，无需操作，直接返回。
     *
     *  !pStream && !pSrc  → EOS：所有帧已编码完毕，通知 flush() 等待者。
     *
     *   pStream &&  pSrc  → 正常帧编码完成：处理码流数据，然后调用
     *                        AL_Encoder_PutStreamBuffer 将流缓冲重新入队，
     *                        供编码器编码下一帧使用（注意：此处不调用 Unref）。
     */

    /* source-release：SDK 释放自身对源帧缓冲的引用。
     * 对内部池缓冲无需干预；对外部 dmabuf 缓冲需要通知调用方回收。 */
    if (!pStream && pSrc)
    {
        releaseExternSources(pSrc);
        return;
    }

    /* stream-release：仅通知，无需处理 */
    if (pStream && !pSrc)
    {
        return;
    }

    /* EOS 信号 */
    if (!pStream && !pSrc)
    {
        std::unique_lock<std::mutex> lock(m_eosMutex);
        m_eosReceived = true;
        m_eosCv.notify_all();
        return;
    }

    /* 帧编码完成：pStream != nullptr && pSrc != nullptr */
    AL_ERR const encErr = AL_Encoder_GetLastError(m_hEnc);
    if (AL_IS_ERROR_CODE(encErr))
    {
        VIDEO_ERROR_PRINT("RealtimeEncoder: encoder error: %s", AL_Codec_ErrorToString(encErr));
        m_hasError.store(true);
    }
    else if (AL_IS_WARNING_CODE(encErr))
    {
        VIDEO_INFO_PRINT("RealtimeEncoder: encoder warning: %s", AL_Codec_ErrorToString(encErr));
    }

    auto *pMeta = reinterpret_cast<AL_TStreamMetaData *>(AL_Buffer_GetMetaData(pStream, AL_META_TYPE_STREAM));

    if (pMeta && pMeta->uNumSection > 0)
    {
        const auto isKeyFrame =
            std::any_of(pMeta->pSections, pMeta->pSections + pMeta->uNumSection,
                        [](const AL_TStreamSection &sec) { return (sec.eFlags & AL_SECTION_SYNC_FLAG) != 0; });
        const auto *pBase = AL_Buffer_GetData(pStream);
        const auto uSize = ReconstructStream(m_dmaProxy, pStream, 0);

        if (m_callback && uSize)
        {
            try
            {
                m_callback(pBase, uSize, isKeyFrame);
            }
            catch (...)
            {
                m_hasError.store(true);
            }
        }
    }

    if (pMeta)
    {
        AL_StreamMetaData_ClearAllSections(pMeta);
    }

    if (!AL_Encoder_PutStreamBuffer(m_hEnc, pStream))
    {
        VIDEO_ERROR_PRINT("Failed to put stream buffer back to encoder");
        m_hasError.store(true);
    }
}

void RealtimeEncoder::initSettings(AL_TEncSettings &settings) const
{
    AL_TEncChanParam &ch = settings.tChParam[0];

    /* --- 分辨率 --- */
    ch.uEncWidth = m_cfg.width;
    ch.uEncHeight = m_cfg.height;
    ch.uSrcWidth = m_cfg.width;
    ch.uSrcHeight = m_cfg.height;

    /* --- 编码格式 --- */
    ch.eProfile = m_cfg.eProfile;
    ch.uLevel = m_cfg.uLevel;
    ch.uTier = m_cfg.uTier;
    AL_SET_CHROMA_MODE(&ch.ePicFormat, m_cfg.eChromaMode);
    AL_SET_BITDEPTH(&ch.ePicFormat, m_cfg.uBitDepth);
    ch.uSrcBitDepth = m_cfg.uBitDepth;
    ch.eSrcMode = AL_SRC_RASTER;
    ch.eVideoMode = AL_VM_PROGRESSIVE;

    /* --- 工具开关：使能环路滤波（跨 Slice） --- */
    ch.eEncTools = static_cast<AL_EChEncTool>(AL_OPT_LF | AL_OPT_LF_X_SLICE);

    /* --- 码率控制 --- */
    AL_TRCParam &rc = ch.tRCParam;
    rc.eRCMode = m_cfg.eRCMode;
    rc.uTargetBitRate = m_cfg.uTargetBitRate;
    rc.uMaxBitRate = (m_cfg.uMaxBitRate > 0) ? m_cfg.uMaxBitRate : m_cfg.uTargetBitRate;
    rc.uFrameRate = m_cfg.uFrameRate;
    rc.uClkRatio = m_cfg.uClkRatio;
    rc.iInitialQP = m_cfg.iInitialQP;

    /* CPB 大小 = 目标码率 / 帧率 */
    float fps = static_cast<float>(m_cfg.uFrameRate) * 1000.0f / static_cast<float>(m_cfg.uClkRatio);
    rc.uCPBSize = static_cast<uint32_t>(m_cfg.uTargetBitRate / fps);
    rc.uInitialRemDelay = rc.uCPBSize;

    /* --- GOP 结构 --- */
    AL_TGopParam &gop = ch.tGopParam;
    if (m_cfg.bLowDelayMode)
    {
        gop.eMode = AL_GOP_MODE_LOW_DELAY_P;
        gop.uNumB = 0;
        // ch.uNumSlices = 8;
        // ch.uSliceSize = 0;
        // ch.eEncOptions = static_cast<AL_EChEncOption>(ch.eEncOptions | AL_OPT_LOWLAT_INT);
    }
    else
    {
        gop.eMode = AL_GOP_MODE_DEFAULT;
        gop.uNumB = m_cfg.uNumB;
    }
    gop.uGopLength = m_cfg.uGopLength;
    gop.uFreqIDR = (m_cfg.uFreqIDR > 0) ? m_cfg.uFreqIDR : m_cfg.uGopLength;

    /* --- 全局设置 --- */
    settings.bEnableAUD = false;
    settings.eEnableFillerData = AL_FILLER_DISABLE;
    settings.NumLayer = 1;
    settings.LookAhead = 0;
    settings.TwoPass = 0;
    settings.eQpTableMode = AL_QP_TABLE_NONE;
}

void RealtimeEncoder::initSrcBufPool()
{
    AL_TDimension tDim{static_cast<int32_t>(m_cfg.width), static_cast<int32_t>(m_cfg.height)};

    /* 计算各平面描述 */
    AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES]{};
    int nPlanes = AL_Plane_GetBufferPixelPlanes(m_picFormat, usedPlanes);

    std::vector<AL_TPlaneDescription> planeDescs;
    int chunkSize = 0;

    for (int i = 0; i < nPlanes; ++i)
    {
        int pitch = (usedPlanes[i] == AL_PLANE_Y) ? m_pitchY : AL_GetChromaPitch(m_srcFourCC, m_pitchY);
        planeDescs.push_back(AL_TPlaneDescription{usedPlanes[i], chunkSize, pitch});
        chunkSize += static_cast<int>(AL_GetAllocSizeSrc_PixPlane(&m_picFormat, m_pitchY, m_strideH, usedPlanes[i]));
    }

    m_srcBufPool->SetFormat(tDim, m_srcFourCC);
    m_srcBufPool->AddChunk(static_cast<size_t>(chunkSize), planeDescs);

    if (!m_srcBufPool->Init(m_pAllocator, m_cfg.uNumSrcBufs, "src-frame"))
        throw std::runtime_error("Failed to initialize source buffer pool");
}

void RealtimeEncoder::initStreamBufPool()
{
    AL_TDimension tDim{static_cast<int32_t>(m_cfg.width), static_cast<int32_t>(m_cfg.height)};

    /* 流缓冲大小 = 最大 NAL 大小估算（保守模式） + 头部空间 */
    uint64_t streamSize = static_cast<uint64_t>(AL_GetMitigatedMaxNalSize(tDim, m_cfg.eChromaMode, m_cfg.uBitDepth));
    streamSize += AL_ENC_MAX_HEADER_SIZE;

    /* 流缓冲数量 = 基础数量 + GOP 内 B 帧数（防止流水线饥饿） */
    uint32_t numBufs =
        m_cfg.uNumStreamBufs + static_cast<uint32_t>(kStreamSmoothingCount) + static_cast<uint32_t>(m_cfg.uNumB);

    /* 带 Stream Metadata 的元数据附加到所有缓冲 */
    auto *pMeta = reinterpret_cast<AL_TMetaData *>(AL_StreamMetaData_Create(AL_MAX_SECTION));
    if (!pMeta)
        throw std::runtime_error("Failed to create stream metadata template");

    bool ok = m_streamBufPool->Init(m_pAllocator, numBufs, static_cast<size_t>(streamSize), pMeta, "stream");
    AL_MetaData_Destroy(pMeta);

    if (!ok)
        throw std::runtime_error("Failed to initialize stream buffer pool");
}

void RealtimeEncoder::pushStreamBuffers()
{
    /* 将所有流缓冲预先入队到编码器，编码器将按需使用 */
    uint32_t total =
        m_cfg.uNumStreamBufs + static_cast<uint32_t>(kStreamSmoothingCount) + static_cast<uint32_t>(m_cfg.uNumB);

    for (uint32_t i = 0; i < total; ++i)
    {
        AL_TBuffer *pStream = m_streamBufPool->GetBuffer(AL_BUF_MODE_NONBLOCK);
        if (!pStream)
            break;

        auto *pMeta = reinterpret_cast<AL_TStreamMetaData *>(AL_Buffer_GetMetaData(pStream, AL_META_TYPE_STREAM));
        if (pMeta)
            AL_StreamMetaData_ClearAllSections(pMeta);

        /* 引用计数说明：
         *   - GetBuffer 使 ref=1（用户引用）
         *   - AL_Encoder_PutStreamBuffer 内部 Ref → ref=2（SDK 引用）
         *   - Unref 释放用户引用 → ref=1，缓冲由 SDK 持有 */
        if (!AL_Encoder_PutStreamBuffer(m_hEnc, pStream))
        {
            m_hasError.store(true);
            AL_Buffer_Unref(pStream);
            throw std::runtime_error("Failed to queue stream buffer");
        }
        AL_Buffer_Unref(pStream); /* 释放用户引用；SDK 持有自身引用直到编码回调 */
    }
}

void RealtimeEncoder::releaseExternSources(AL_TBuffer const *pSrc)
{
    if (m_work_mode != WorkMode::V4L2)
    {
        return;
    }

    ExternalSourceContext ctx{};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(m_externalSourcesMutex);
        auto it = m_externalSources.find(pSrc);
        if (it != m_externalSources.end())
        {
            ctx = it->second;
            m_externalSources.erase(it);
            found = true;
        }
    }

    SourceReleaseCallback releaseCB;
    {
        std::lock_guard<std::mutex> lock(m_sourceReleaseCallbackMutex);
        releaseCB = m_sourceReleaseCallback;
    }

    if (found && releaseCB)
    {
        try
        {
            m_sourceReleaseCallback(ctx.token);
        }
        catch (...)
        {
            m_hasError.store(true);
        }
    }
}
