// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "RealtimeEncoder.h"

#include <chrono>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>
#include <utility>

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
}

/* --------------------------------------------------------------------------
 * 内部常量
 * --------------------------------------------------------------------------*/

/** 流缓冲区基础平滑余量（与原工程保持一致） */
static constexpr int kStreamSmoothingCount = 2;
/** 缓冲区高度对齐粒度 */
static constexpr int kHeightAlign = 8;

/* ============================================================================
 * 构造 / 析构
 * ============================================================================*/

RealtimeEncoder::RealtimeEncoder(const EncoderConfig &cfg, EncodedFrameCallback callback)
    : m_cfg(cfg), m_callback(std::move(callback))
{
    if (!m_callback)
        throw std::invalid_argument("RealtimeEncoder: callback must not be null");

    /* 1. 初始化编码器库 */
    AL_ERR err = AL_Lib_Encoder_Init(AL_LIB_ENCODER_ARCH_HOST);
    if (err != AL_SUCCESS)
        throw std::runtime_error(std::string("AL_Lib_Encoder_Init failed: ") + AL_Codec_ErrorToString(err));

    /* 2. 创建 DMA 分配器（访问 ZYNQ VCU 物理内存） */
    m_pAllocator = AL_DmaAlloc_Create(m_cfg.sDevicePath.c_str());
    if (!m_pAllocator)
    {
        AL_Lib_Encoder_DeInit();
        throw std::runtime_error("Failed to create DMA allocator for device: " + m_cfg.sDevicePath);
    }

    /* 3. 创建 MCU 调度器 */
    m_pScheduler = AL_SchedulerMcu_Create(
        AL_GetHardwareDriver(), reinterpret_cast<AL_TLinuxDmaAllocator *>(m_pAllocator), m_cfg.sDevicePath.c_str());
    if (!m_pScheduler)
    {
        AL_Allocator_Destroy(m_pAllocator);
        m_pAllocator = nullptr;
        AL_Lib_Encoder_DeInit();
        throw std::runtime_error("Failed to create MCU scheduler");
    }

    /* 4. 缓存源图像格式信息 */
    m_picFormat = AL_EncGetSrcPicFormat(m_cfg.eChromaMode, m_cfg.uBitDepth, AL_SRC_RASTER);
    m_srcFourCC = AL_EncGetSrcFourCC(m_picFormat);
    m_pitchY = AL_EncGetMinPitch(m_cfg.width, &m_picFormat);
    m_strideH = AL_RoundUp(static_cast<int>(m_cfg.height), kHeightAlign);

    /* 5. 构建编码器参数并创建编码器 */
    AL_TEncSettings settings{};
    AL_Settings_SetDefaults(&settings);
    initSettings(settings);

    {
        int nInvalid = AL_Settings_CheckValidity(&settings, &settings.tChParam[0], stderr);
        if (nInvalid > 0)
            throw std::runtime_error("AL_Settings_CheckValidity found " + std::to_string(nInvalid) +
                                     " invalid parameter(s)");
    }
    {
        int nIncoherent = AL_Settings_CheckCoherency(&settings, &settings.tChParam[0], m_srcFourCC, stderr);
        if (nIncoherent < 0)
            throw std::runtime_error("AL_Settings_CheckCoherency: fatal incoherency");
    }

    AL_CB_EndEncoding cb{};
    cb.func = &RealtimeEncoder::sdkCallback;
    cb.userParam = this;

    err = AL_Encoder_Create(&m_hEnc, m_pScheduler, m_pAllocator, &settings, cb);
    if (err != AL_SUCCESS || m_hEnc == nullptr)
    {
        AL_IEncScheduler_Destroy(m_pScheduler);
        m_pScheduler = nullptr;
        AL_Allocator_Destroy(m_pAllocator);
        m_pAllocator = nullptr;
        AL_Lib_Encoder_DeInit();
        throw std::runtime_error(std::string("AL_Encoder_Create failed: ") + AL_Codec_ErrorToString(err));
    }

    /* 6. 创建并初始化缓冲区池 */
    m_srcBufPool = std::make_unique<PixMapBufPool>();
    m_streamBufPool = std::make_unique<BufPool>();
    initSrcBufPool();
    initStreamBufPool();

    /* 7. 预先向编码器填充全部流缓冲 */
    pushStreamBuffers();
}

RealtimeEncoder::~RealtimeEncoder()
{
    /* 若调用者未主动 flush，确保 EOS 被发送 */
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

    /* 1. 先销毁编码器（停止使用buffer） */
    if (m_hEnc != nullptr)
    {
        AL_Encoder_Destroy(m_hEnc);
        m_hEnc = nullptr;
    }

    /* 2. 显式释放 buffer pools（必须在 allocator 销毁之前）
     * BufPool 析构时会调用 AL_Buffer_Destroy 释放所有 DMA buffer，
     * 这需要使用 allocator。因此我们必须在销毁 allocator 之前
     * 先销毁 buffer pool。 */
    if (m_srcBufPool)
    {
        m_srcBufPool->Decommit();
        m_srcBufPool.reset(); // 显式释放，调用 ~PixMapBufPool()
    }
    if (m_streamBufPool)
    {
        m_streamBufPool->Decommit();
        m_streamBufPool.reset(); // 显式释放，调用 ~BufPool()
    }

    /* 3. 现在可以安全地销毁 scheduler 和 allocator */
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

/* ============================================================================
 * 公开接口
 * ============================================================================*/

bool RealtimeEncoder::pushFrame(const uint8_t *pData, int srcPitch)
{
    if (!pData)
        return false;

    if (m_stopped.load())
        return false;

    if (m_hasError.load())
        return false;

    /* 从源缓冲池取一块空闲 DMA 缓冲（阻塞直到有空闲） */
    AL_TBuffer *pSrcBuf = m_srcBufPool->GetBuffer(AL_BUF_MODE_BLOCK);
    if (!pSrcBuf)
        return false;

    /* 将调用者提供的帧数据拷贝到 DMA 缓冲 */
    copyFrameData(pSrcBuf, pData, srcPitch);

    /* 提交给硬件编码
     * 引用计数说明：
     *   - GetBuffer 已使 ref=1（用户引用）
     *   - AL_Encoder_Process 内部再 Ref → ref=2（SDK 引用）
     *   - 我们立即 Unref 释放用户引用 → ref=1，缓冲由 SDK 持有
     *   - SDK 编码完毕后触发 source-release 回调并 Unref → ref=0 → 回池 */
    bool const ok = AL_Encoder_Process(m_hEnc, pSrcBuf, nullptr);
    AL_Buffer_Unref(pSrcBuf); /* 释放用户引用；SDK 持有自身引用直到编码完毕 */

    if (!ok)
    {
        m_hasError.store(true);
        return false;
    }
    return true;
}

AL_TBuffer *RealtimeEncoder::acquireSourceBuffer()
{
    if (m_stopped.load() || m_hasError.load())
        return nullptr;
    return m_srcBufPool->GetBuffer(AL_BUF_MODE_BLOCK);
}

bool RealtimeEncoder::submitSourceBuffer(AL_TBuffer *pBuf)
{
    if (!pBuf)
        return false;

    if (m_stopped.load() || m_hasError.load())
    {
        /* 缓冲已由调用方持有（ref=1），需要释放避免泄漏 */
        AL_Buffer_Unref(pBuf);
        return false;
    }

    /* 引用计数说明：
     *   - acquireSourceBuffer 内 GetBuffer 已使 ref=1（用户引用）
     *   - AL_Encoder_Process 内部 Ref → ref=2（SDK 引用）
     *   - 此处 Unref 释放用户引用 → ref=1，缓冲由 SDK 持有
     *   - 编码完毕后经 source-release 回调自动回池 */
    bool const ok = AL_Encoder_Process(m_hEnc, pBuf, nullptr);
    AL_Buffer_Unref(pBuf); /* 释放用户引用 */

    if (!ok)
    {
        m_hasError.store(true);
        return false;
    }
    return true;
}

bool RealtimeEncoder::submitDmabufFd(int fd, size_t size, uint64_t token)
{
    if (fd < 0 || size == 0)
        return false;

    if (m_stopped.load() || m_hasError.load())
        return false;

    auto *pLinuxAllocator = reinterpret_cast<AL_TLinuxDmaAllocator *>(m_pAllocator);
    AL_HANDLE dmaHandle = AL_LinuxDmaAllocator_ImportFromFd(pLinuxAllocator, fd);
    if (!dmaHandle)
        return false;

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
        return; /* 已经调用过 flush */

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
    static constexpr auto kEosWaitTimeout = std::chrono::seconds(10);
    std::unique_lock<std::mutex> lock(m_eosMutex);
    bool const gotEos = m_eosCv.wait_for(lock, kEosWaitTimeout, [this] { return m_eosReceived; });
    if (!gotEos)
    {
        m_hasError.store(true);
        throw std::runtime_error("flush timeout: EOS callback not received");
    }
}

void RealtimeEncoder::requestKeyFrame()
{
    if (m_hEnc != nullptr)
        AL_Encoder_RestartGop(m_hEnc);
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

/* ============================================================================
 * SDK 回调
 * ============================================================================*/

/*static*/
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

        SourceReleaseCallback releaseCb;
        {
            std::lock_guard<std::mutex> lock(m_sourceReleaseCallbackMutex);
            releaseCb = m_sourceReleaseCallback;
        }

        if (found && releaseCb)
        {
            try
            {
                releaseCb(ctx.token);
            }
            catch (...)
            {
                m_hasError.store(true);
            }
        }

        return;
    }

    /* stream-release：仅通知，无需处理 */
    if (pStream && !pSrc)
        return;

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
        std::fprintf(stderr, "RealtimeEncoder: encoder error: %s\n", AL_Codec_ErrorToString(encErr));
        m_hasError.store(true);
    }

    if (AL_IS_WARNING_CODE(encErr))
    {
        std::fprintf(stderr, "RealtimeEncoder: encoder warning: %s\n", AL_Codec_ErrorToString(encErr));
    }

    auto *pMeta = reinterpret_cast<AL_TStreamMetaData *>(AL_Buffer_GetMetaData(pStream, AL_META_TYPE_STREAM));

    if (pMeta && pMeta->uNumSection > 0 && m_callback)
    {
        const uint8_t *pBase = AL_Buffer_GetData(pStream);
        size_t const streamCapacity = pStream->zSizes[0];

        /* 检查是否含有同步帧（IDR/I-frame）标志 */
        bool isKeyFrame = false;
        for (uint16_t i = 0; i < pMeta->uNumSection; ++i)
        {
            if (pMeta->pSections[i].eFlags & AL_SECTION_SYNC_FLAG)
            {
                isKeyFrame = true;
                break;
            }
        }

        /* 将所有 section 重组为连续码流，保持与 OMX 侧重组语义一致。 */
        std::vector<uint8_t> frameData;

        size_t totalPayload = 0;
        for (uint16_t i = 0; i < pMeta->uNumSection; ++i)
        {
            const AL_TStreamSection &sec = pMeta->pSections[i];
            if (sec.uLength == 0 || (sec.eFlags & AL_SECTION_END_FRAME_FLAG))
                continue;
            totalPayload += sec.uLength;
        }
        frameData.reserve(totalPayload);

        for (uint16_t i = 0; i < pMeta->uNumSection; ++i)
        {
            const AL_TStreamSection &sec = pMeta->pSections[i];
            if (sec.uLength == 0 || (sec.eFlags & AL_SECTION_END_FRAME_FLAG))
                continue;

            size_t const sectionOffset = sec.uOffset;
            size_t const sectionLength = sec.uLength;

            if (sectionOffset >= streamCapacity)
            {
                m_hasError.store(true);
                continue;
            }

            size_t const safeLength = std::min(sectionLength, streamCapacity);

            if (sectionOffset + safeLength <= streamCapacity)
            {
                frameData.insert(frameData.end(), pBase + sectionOffset, pBase + sectionOffset + safeLength);
            }
            else
            {
                size_t const firstPart = streamCapacity - sectionOffset;
                frameData.insert(frameData.end(), pBase + sectionOffset, pBase + sectionOffset + firstPart);
                frameData.insert(frameData.end(), pBase, pBase + (safeLength - firstPart));
            }
        }

        if (!frameData.empty())
        {
            try
            {
                m_callback(frameData.data(), frameData.size(), isKeyFrame);
            }
            catch (...)
            {
                m_hasError.store(true);
            }
        }
    }

    /* 重置元数据节计数，将流缓冲重新入队给编码器（不调用 Unref！） */
    if (pMeta)
        AL_StreamMetaData_ClearAllSections(pMeta);

    bool const putOk = AL_Encoder_PutStreamBuffer(m_hEnc, pStream);
    if (!putOk)
        m_hasError.store(true);
}

/* ============================================================================
 * 初始化辅助
 * ============================================================================*/

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
    ch.ePicFormat = static_cast<AL_EPicFormat>((static_cast<int>(m_cfg.eChromaMode) << 8) | (m_cfg.uBitDepth & 0x0F) |
                                               ((m_cfg.uBitDepth & 0x0F) << 4));
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

    /* CPB 大小 = 目标码率 / 帧率（1 秒缓冲） */
    float fps = static_cast<float>(m_cfg.uFrameRate) * 1000.0f / static_cast<float>(m_cfg.uClkRatio);
    rc.uCPBSize = static_cast<uint32_t>(m_cfg.uTargetBitRate / fps);
    rc.uInitialRemDelay = rc.uCPBSize;

    /* --- GOP 结构 --- */
    AL_TGopParam &gop = ch.tGopParam;
    if (m_cfg.bLowDelayMode)
    {
        gop.eMode = AL_GOP_MODE_LOW_DELAY_P;
        gop.uNumB = 0;
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
        bool const putOk = AL_Encoder_PutStreamBuffer(m_hEnc, pStream);
        if (!putOk)
        {
            m_hasError.store(true);
            AL_Buffer_Unref(pStream);
            throw std::runtime_error("Failed to queue stream buffer");
        }
        AL_Buffer_Unref(pStream); /* 释放用户引用；SDK 持有自身引用直到编码回调 */
    }
}

/* ============================================================================
 * 帧数据拷贝
 * ============================================================================*/

void RealtimeEncoder::copyFrameData(AL_TBuffer *pDstBuf, const uint8_t *pSrcData, int srcPitch) const
{
    /* ---- 亮度平面 ---- */
    uint8_t *pDstY = AL_PixMapBuffer_GetPlaneAddress(pDstBuf, AL_PLANE_Y);
    int dstPitchY = AL_PixMapBuffer_GetPlanePitch(pDstBuf, AL_PLANE_Y);

    /* 如果调用者未指定 srcPitch，则推断为紧凑布局 */
    int bytesPerSample = (m_cfg.uBitDepth > 8) ? 2 : 1;
    int defaultPitch = static_cast<int>(m_cfg.width) * bytesPerSample;
    int actualSrcPitch = (srcPitch > 0) ? srcPitch : defaultPitch;

    /* 逐行复制亮度 */
    const uint8_t *pSrcY = pSrcData;
    for (int row = 0; row < static_cast<int>(m_cfg.height); ++row)
    {
        std::memcpy(pDstY + row * dstPitchY, pSrcY + row * actualSrcPitch, static_cast<size_t>(defaultPitch));
    }

    /* ---- 色度平面（仅处理 YUV420/422/444 semi-planar 和 planar 两种情况） ---- */
    if (m_cfg.eChromaMode == AL_CHROMA_MONO)
        return;

    /* 源数据：Y 平面之后紧接色度数据 */
    int lumaLines = static_cast<int>(m_cfg.height);
    const uint8_t *pSrcChroma = pSrcData + lumaLines * actualSrcPitch;

    /* 色度行数 */
    int chromaLines = (m_cfg.eChromaMode == AL_CHROMA_4_2_0) ? (lumaLines / 2) : lumaLines;
    int chromaBytesPerRow = (m_cfg.eChromaMode == AL_CHROMA_4_4_4) ? defaultPitch * 2 /* U + V，各一行 */
                                                                   : defaultPitch;    /* UV 交织或 U/V 分离 */

    /* 检查是否为 semi-planar（NV12/NV16/P010 等） */
    uint8_t *pDstUV = AL_PixMapBuffer_GetPlaneAddress(pDstBuf, AL_PLANE_UV);
    if (pDstUV)
    {
        /* Semi-planar: UV 交织，色度行宽 = 亮度行宽 */
        int dstPitchUV = AL_PixMapBuffer_GetPlanePitch(pDstBuf, AL_PLANE_UV);
        int srcChromaRowBytes = defaultPitch; /* U 和 V 已交织，宽度等于亮度宽 */
        (void)chromaBytesPerRow;

        for (int row = 0; row < chromaLines; ++row)
        {
            std::memcpy(pDstUV + row * dstPitchUV, pSrcChroma + row * srcChromaRowBytes,
                        static_cast<size_t>(srcChromaRowBytes));
        }
    }
    else
    {
        /* Planar: U 和 V 分开 */
        uint8_t *pDstU = AL_PixMapBuffer_GetPlaneAddress(pDstBuf, AL_PLANE_U);
        uint8_t *pDstV = AL_PixMapBuffer_GetPlaneAddress(pDstBuf, AL_PLANE_V);
        int dstPitchU = AL_PixMapBuffer_GetPlanePitch(pDstBuf, AL_PLANE_U);
        int dstPitchV = AL_PixMapBuffer_GetPlanePitch(pDstBuf, AL_PLANE_V);

        int uvWidth = (m_cfg.eChromaMode == AL_CHROMA_4_4_4) ? defaultPitch : defaultPitch / 2;

        const uint8_t *pSrcU = pSrcChroma;
        const uint8_t *pSrcV = pSrcChroma + chromaLines * uvWidth;

        for (int row = 0; row < chromaLines; ++row)
        {
            if (pDstU)
                std::memcpy(pDstU + row * dstPitchU, pSrcU + row * uvWidth, static_cast<size_t>(uvWidth));
            if (pDstV)
                std::memcpy(pDstV + row * dstPitchV, pSrcV + row * uvWidth, static_cast<size_t>(uvWidth));
        }
    }
}
