#include "RTEncoder.h"

extern "C"
{
#include "lib_common/BufferStreamMeta.h"
#include "lib_common/HardwareDriver.h"
#include "lib_common/RoundUp.h"
#include "lib_common/StreamBuffer.h"
#include "lib_common_enc/EncBuffers.h"
#include "lib_common_enc/IpEncFourCC.h"
#include "lib_encode/EncSchedulerMcu.h"
#include "lib_fpga/DmaAlloc.h"
#include "lib_fpga/DmaAllocLinux.h"
#include "lib_rtos/message.h"
}

#include <algorithm>

static constexpr int kStreamSmoothingCount = 2;
static constexpr int kHeightAlign = 8;
static constexpr int kMaxSliceNum = 8;
static constexpr uint32_t kMaxWidth = 3840;
static constexpr uint32_t kMaxHeight = 2160;

using AL_BufferGuard = std::unique_ptr<AL_TBuffer, decltype(&AL_Buffer_Unref)>;

static size_t build_pixmap_plane_descs(const AL_TPicFormat &pic_fmt, TFourCC fourcc, AL_TDimension src_dim,
                                       std::vector<AL_TPlaneDescription> &plane_descs)
{
    plane_descs.clear();
    AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES]{};
    auto nPlanes = AL_Plane_GetBufferPixelPlanes(pic_fmt, usedPlanes);
    auto pitchY = AL_EncGetMinPitch(src_dim.iWidth, &pic_fmt);
    auto strideH = AL_RoundUp(src_dim.iHeight, kHeightAlign);

    size_t chunk_size = 0;
    for (int i = 0; i < nPlanes; i++)
    {
        auto pitch = (usedPlanes[i] == AL_PLANE_Y) ? pitchY : AL_GetChromaPitch(fourcc, pitchY);
        plane_descs.push_back({usedPlanes[i], static_cast<int>(chunk_size), pitch});
        chunk_size += static_cast<int>(AL_GetAllocSizeSrc_PixPlane(&pic_fmt, pitchY, strideH, usedPlanes[i]));
    }
    return chunk_size;
}

static uint32_t write_one_section(DMAProxy &mem_ctl, AL_TBuffer *source, AL_TBuffer *destination, int offset,
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

static uint32_t write_filler_data_section(DMAProxy &mem_ctl, AL_TBuffer *source, AL_TBuffer *destination, int offset,
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

static uint32_t reconstruct_stream(DMAProxy &mem_ctl, AL_TBuffer *stream, int firstSection, bool &eof)
{
    uint32_t size = 0;
    auto meta = (AL_TStreamMetaData *)(AL_Buffer_GetMetaData(stream, AL_META_TYPE_STREAM));

    DBG_ASSERT_COND(meta != nullptr);
    DBG_ASSERT_LE(firstSection, meta->uNumSection);

    for (int i = firstSection; i < meta->uNumSection; i++)
    {
        if (meta->pSections[i].eFlags & AL_SECTION_APP_FILLER_FLAG)
        {
            size += write_filler_data_section(mem_ctl, stream, stream, static_cast<int>(size), i);
        }
        else
        {
            size += write_one_section(mem_ctl, stream, stream, static_cast<int>(size), i);
        }

        if (!eof && meta->pSections[i].eFlags & AL_SECTION_END_FRAME_FLAG)
        {
            eof = true;
        }
    }

    return size;
}

RTEncoderBase::RTEncoderBase(const EncoderConfig &cfg, EncodedFrameCallback cb)

    : m_fps(0.0), m_bitrate(0), m_frame_count(0), m_bytes_count(0), m_fps_last_time(std::chrono::steady_clock::now()),
      m_cfg(cfg), m_callback(std::move(cb)), m_dma_proxy(cfg.dma_dev_path.c_str()), m_pAllocator(nullptr),
      m_pScheduler(nullptr), m_pic_format{}, m_src_fourcc{}, m_stopped(false), m_eos_signaled(false), m_error(false),
      m_lib_initialized(false)
{
    try
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
        m_lib_initialized = true;

        m_pAllocator = AL_DmaAlloc_Create(m_cfg.enc_dev_path.c_str());
        if (!m_pAllocator)
        {
            throw std::runtime_error("Failed to create DMA allocator for device: " + m_cfg.enc_dev_path);
        }

        m_pScheduler =
            AL_SchedulerMcu_Create(AL_GetHardwareDriver(), reinterpret_cast<AL_TLinuxDmaAllocator *>(m_pAllocator),
                                   m_cfg.enc_dev_path.c_str());
        if (!m_pScheduler)
        {
            throw std::runtime_error("Failed to create MCU scheduler");
        }

        m_pic_format = AL_EncGetSrcPicFormat(m_cfg.chroma_mode, m_cfg.bit_depth, AL_SRC_RASTER);
        m_src_fourcc = AL_EncGetSrcFourCC(m_pic_format);

        AL_TEncSettings settings{};
        AL_Settings_SetDefaults(&settings);
        AL_Settings_SetDefaultParam(&settings);
        init_settings(settings);

        auto result = AL_Settings_CheckValidity(&settings, &settings.tChParam[0], nullptr);
        if (result > 0)
        {
            throw std::runtime_error("AL_Settings_CheckValidity found " + std::to_string(result) +
                                     " invalid parameter(s)");
        }

        result = AL_Settings_CheckCoherency(&settings, &settings.tChParam[0], m_src_fourcc, nullptr);
        if (result < 0)
        {
            throw std::runtime_error("AL_Settings_CheckCoherency: fatal incoherency");
        }

        err = AL_Encoder_Create(&m_hEnc, m_pScheduler, m_pAllocator, &settings, {&RTEncoderBase::sdk_callback, this});
        if (err != AL_SUCCESS || m_hEnc == nullptr)
        {
            throw std::runtime_error(std::string("AL_Encoder_Create failed: ") + AL_Codec_ErrorToString(err));
        }

        if (!AL_Encoder_SetAutoQP(m_hEnc, true))
        {
            VIDEO_ERROR_PRINT("AL_Encoder_SetAutoQP failed: %s",
                              AL_Codec_ErrorToString(AL_Encoder_GetLastError(m_hEnc)));
        }

        init_source_buf_pool();
        init_stream_buf_pool();
        push_stream_buffers();
        set_resolution(m_cfg.width, m_cfg.height);
    }
    catch (...)
    {
        if (m_hEnc)
        {
            AL_Encoder_Destroy(m_hEnc);
            m_hEnc = nullptr;
        }

        if (m_source_buf_pool)
        {
            m_source_buf_pool->decommit();
            m_source_buf_pool.reset();
        }

        if (m_stream_buf_pool)
        {
            m_stream_buf_pool->decommit();
            m_stream_buf_pool.reset();
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

        if (m_lib_initialized)
        {
            AL_Lib_Encoder_DeInit();
            m_lib_initialized = false;
        }

        throw;
    }
}

RTEncoderBase::~RTEncoderBase()
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

    if (m_hEnc)
    {
        AL_Encoder_Destroy(m_hEnc);
        m_hEnc = nullptr;
    }

    if (m_source_buf_pool)
    {
        m_source_buf_pool->decommit();
        m_source_buf_pool.reset();
    }

    if (m_stream_buf_pool)
    {
        m_stream_buf_pool->decommit();
        m_stream_buf_pool.reset();
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

    if (m_lib_initialized)
    {
        AL_Lib_Encoder_DeInit();
        m_lib_initialized = false;
    }
}

void RTEncoderBase::flush()
{
    if (m_stopped.exchange(true))
    {
        return; // Already stopped
    }

    if (m_source_buf_pool)
    {
        m_source_buf_pool->decommit();
    }

    const auto eos_queued = AL_Encoder_Process(m_hEnc, nullptr, nullptr);
    if (!eos_queued)
    {
        m_error.store(true);
        throw std::runtime_error("flush failed: unable to queue EOS");
    }

    std::unique_lock<std::mutex> lock(m_eos_mutex);
    const auto got_eos = m_eos_cond.wait_for(lock, std::chrono::seconds(10), [this] { return m_eos_signaled; });
    if (!got_eos)
    {
        m_error.store(true);
        throw std::runtime_error("flush timeout: EOS callback not received");
    }
}

void RTEncoderBase::request_IDR()
{
    if (m_hEnc)
    {
        AL_Encoder_RestartGop(m_hEnc);
    }
}

bool RTEncoderBase::set_bitrate(uint32_t uTargetBitRate, uint32_t uMaxBitRate)
{
    if (!m_hEnc)
    {
        return false;
    }

    if (uTargetBitRate == 0)
    {
        return false;
    }

    const uint32_t effectiveMax = (uMaxBitRate > 0) ? uMaxBitRate : uTargetBitRate;
    const int targetKbps = static_cast<int>((uTargetBitRate + 999U) / 1000U);
    const int maxKbps = static_cast<int>((effectiveMax + 999U) / 1000U);

    bool ok = false;
    if (effectiveMax != uTargetBitRate)
    {
        ok = AL_Encoder_SetMaxBitRate(m_hEnc, targetKbps, maxKbps);
    }
    else
    {
        ok = AL_Encoder_SetBitRate(m_hEnc, targetKbps);
    }

    if (!ok)
    {
        VIDEO_ERROR_PRINT("set_bitrate failed: %s", AL_Codec_ErrorToString(AL_Encoder_GetLastError(m_hEnc)));
        m_error.store(true);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_cfg_mutex);
    m_cfg.target_bitrate = uTargetBitRate;
    m_cfg.max_bitrate = effectiveMax;

    return true;
}

bool RTEncoderBase::set_framerate(uint32_t uFrameRate, uint32_t uClkRatio)
{
    if (!m_hEnc || uFrameRate == 0 || uClkRatio == 0)
    {
        return false;
    }

    if (!AL_Encoder_SetFrameRate(m_hEnc, uFrameRate, uClkRatio))
    {
        VIDEO_ERROR_PRINT("set_framerate failed: %s", AL_Codec_ErrorToString(AL_Encoder_GetLastError(m_hEnc)));
        m_error.store(true);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_cfg_mutex);
    m_cfg.framerate = uFrameRate;
    m_cfg.clk_ratio = uClkRatio;

    return true;
}

bool RTEncoderBase::set_resolution(uint32_t uWidth, uint32_t uHeight)
{
    if (!m_hEnc || uWidth == 0 || uHeight == 0)
    {
        return false;
    }

    if (uWidth > kMaxWidth || uHeight > kMaxHeight)
    {
        VIDEO_ERROR_PRINT("set_resolution rejected: %ux%u exceeds init/max %ux%u", uWidth, uHeight, kMaxWidth,
                          kMaxHeight);
        return false;
    }

    const AL_TDimension dim{static_cast<int32_t>(uWidth), static_cast<int32_t>(uHeight)};
    if (!AL_Encoder_SetInputResolution(m_hEnc, dim))
    {
        VIDEO_ERROR_PRINT("set_resolution failed: %s", AL_Codec_ErrorToString(AL_Encoder_GetLastError(m_hEnc)));
        m_error.store(true);
        return false;
    }

    std::lock_guard<std::mutex> lock(m_cfg_mutex);
    m_cfg.width = uWidth;
    m_cfg.height = uHeight;

    return true;
}

TFourCC RTEncoderBase::src_fourCC() const
{
    return m_src_fourcc;
}

uint8_t RTEncoderBase::src_bitdepth() const
{
    return m_cfg.bit_depth;
}

AL_EChromaMode RTEncoderBase::src_chroma() const
{
    return m_cfg.chroma_mode;
}

AL_TDimension RTEncoderBase::src_resolution() const
{
    std::lock_guard<std::mutex> lock(m_cfg_mutex);
    return AL_TDimension{m_cfg.width, m_cfg.height};
}

std::pair<double, double> RTEncoderBase::fps() const
{
    std::lock_guard<std::mutex> lock(m_fps_mutex);
    return {m_fps, m_bitrate};
}

void RTEncoderBase::sdk_callback(void *pUserParam, AL_TBuffer *pStream, AL_TBuffer const *pSrc, int /*iLayerID*/)
{
    auto *pThis = static_cast<RTEncoderBase *>(pUserParam);
    pThis->on_encoded_frame(pStream, pSrc);
}

void RTEncoderBase::on_encoded_frame(AL_TBuffer *pStream, AL_TBuffer const *pSrc)
{
    // source-release
    if (!pStream && pSrc)
    {
        release_sources(pSrc);
        return;
    }

    // stream-release
    if (pStream && !pSrc)
    {
        return;
    }

    // eos
    if (!pStream && !pSrc)
    {
        std::unique_lock<std::mutex> lock(m_eos_mutex);
        m_eos_signaled = true;
        m_eos_cond.notify_all();
        return;
    }

    // normal encoded frame
    AL_ERR const encErr = AL_Encoder_GetLastError(m_hEnc);
    if (AL_IS_ERROR_CODE(encErr))
    {
        VIDEO_ERROR_PRINT("encoder error: %s", AL_Codec_ErrorToString(encErr));
        m_error.store(true);
    }
    else if (AL_IS_WARNING_CODE(encErr))
    {
        VIDEO_INFO_PRINT("encoder warning: %s", AL_Codec_ErrorToString(encErr));
    }

    auto pMeta = reinterpret_cast<AL_TStreamMetaData *>(AL_Buffer_GetMetaData(pStream, AL_META_TYPE_STREAM));
    bool eof = false;
    if (pMeta)
    {
        if (pMeta->uNumSection > 0)
        {
            const auto *pBase = AL_Buffer_GetData(pStream);
            const auto uSize = reconstruct_stream(m_dma_proxy, pStream, 0, eof);
            m_bytes_count += uSize;

            if (m_callback && uSize)
            {
                try
                {
                    m_callback(pBase, uSize);
                }
                catch (...)
                {
                    m_error.store(true);
                }
            }
        }
        AL_StreamMetaData_ClearAllSections(pMeta);
    }

    if (!AL_Encoder_PutStreamBuffer(m_hEnc, pStream))
    {
        VIDEO_ERROR_PRINT("Failed to put stream buffer back to encoder");
        m_error.store(true);
    }

    if (eof)
    {
        release_sources(pSrc);
        update_frame_rate();
    }
}

void RTEncoderBase::update_frame_rate()
{
    if (++m_frame_count % 100 != 0)
    {
        return;
    }

    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - m_fps_last_time).count();
    if (elapsed <= 0)
    {
        return;
    }

    double fps = (m_frame_count * 1000.0) / static_cast<double>(elapsed);
    double bitrate = (m_bytes_count * 8000.0) / static_cast<double>(elapsed);
    {
        std::lock_guard<std::mutex> lock(m_fps_mutex);
        m_fps = 0.1 * m_fps + 0.9 * fps; // EMA with alpha=0.9
        m_bitrate = 0.1 * m_bitrate + 0.9 * bitrate;
    }
    m_fps_last_time = now;
    m_frame_count = 0;
    m_bytes_count = 0;
}

void RTEncoderBase::init_settings(AL_TEncSettings &settings) const
{
    AL_TEncChanParam &ch = settings.tChParam[0];

    ch.uEncWidth = kMaxWidth;
    ch.uEncHeight = kMaxHeight;
    ch.uSrcWidth = kMaxWidth;
    ch.uSrcHeight = kMaxHeight;

    ch.eProfile = m_cfg.profile;
    ch.uLevel = m_cfg.level;
    ch.uTier = m_cfg.tier;
    AL_SET_CHROMA_MODE(&ch.ePicFormat, m_cfg.chroma_mode);
    AL_SET_BITDEPTH(&ch.ePicFormat, m_cfg.bit_depth);
    ch.uSrcBitDepth = m_cfg.bit_depth;
    ch.eSrcMode = AL_SRC_RASTER;
    ch.eVideoMode = AL_VM_PROGRESSIVE;

    ch.eEncTools = static_cast<AL_EChEncTool>(AL_OPT_LF | AL_OPT_LF_X_SLICE);

    AL_TRCParam &rc = ch.tRCParam;
    rc.eRCMode = m_cfg.rc_mode;
    rc.uTargetBitRate = m_cfg.target_bitrate;
    rc.uMaxBitRate = (m_cfg.max_bitrate > 0) ? m_cfg.max_bitrate : m_cfg.target_bitrate;
    rc.uFrameRate = m_cfg.framerate;
    rc.uClkRatio = m_cfg.clk_ratio;
    rc.iInitialQP = m_cfg.initial_qp;

    float fps = static_cast<float>(m_cfg.framerate) * 1000.0f / static_cast<float>(m_cfg.clk_ratio);
    rc.uCPBSize = static_cast<uint32_t>(m_cfg.target_bitrate / fps);
    rc.uInitialRemDelay = rc.uCPBSize;

    AL_TGopParam &gop = ch.tGopParam;
    if (m_cfg.low_delay_mode)
    {
        gop.eMode = AL_GOP_MODE_LOW_DELAY_P;
        gop.uNumB = 0;
        ch.uNumSlices = kMaxSliceNum;
        ch.uSliceSize = 0;
        ch.bSubframeLatency = true;
        rc.eRCMode = AL_RC_LOW_LATENCY;
    }
    else
    {
        gop.eMode = AL_GOP_MODE_DEFAULT;
        gop.uNumB = m_cfg.num_b;
    }
    gop.uGopLength = m_cfg.gop_length;
    gop.uFreqIDR = (m_cfg.freq_idr > 0) ? m_cfg.freq_idr : m_cfg.gop_length;

    settings.bEnableAUD = false;
    settings.eEnableFillerData = AL_FILLER_DISABLE;
    settings.NumLayer = 1;
    settings.LookAhead = 0;
    settings.TwoPass = 0;
    settings.eQpTableMode = AL_QP_TABLE_NONE;
}

void RTEncoderBase::init_source_buf_pool()
{
    // if (m_cfg.low_delay_mode)
    // {
    //     m_cfg.num_src_bufs = std::max(m_cfg.num_src_bufs, static_cast<uint32_t>(kMaxSliceNum + 2));
    // }

    AL_TDimension tDim{static_cast<int32_t>(m_cfg.width), static_cast<int32_t>(m_cfg.height)};
    std::vector<AL_TPlaneDescription> plane_descs;
    size_t chunk_size = build_pixmap_plane_descs(m_pic_format, m_src_fourcc, tDim, plane_descs);

    m_source_buf_pool = std::make_unique<PixMapBufPool>();
    m_source_buf_pool->set_format(tDim, m_src_fourcc);
    m_source_buf_pool->add_chunk(chunk_size, plane_descs);

    if (!m_source_buf_pool->init(m_pAllocator, m_cfg.num_src_bufs, "source_pool"))
    {
        throw std::runtime_error("Failed to initialize source buffer pool");
    }

    VIDEO_INFO_PRINT("Source buffer pool initialized: format=%s, size=%ux%u, planes=%zu, chunk_size=%zu, num_bufs=%u",
                     FOURCC2STR(m_src_fourcc).c_str(), tDim.iWidth, tDim.iHeight, plane_descs.size(), chunk_size,
                     m_cfg.num_src_bufs);
}

void RTEncoderBase::init_stream_buf_pool()
{
    if (m_cfg.low_delay_mode)
    {
        m_cfg.num_stream_bufs = std::max(m_cfg.num_stream_bufs, static_cast<uint32_t>(kMaxSliceNum + 2));
    }

    AL_TDimension tDim{static_cast<int32_t>(m_cfg.width), static_cast<int32_t>(m_cfg.height)};
    uint64_t streamSize = static_cast<uint64_t>(AL_GetMitigatedMaxNalSize(tDim, m_cfg.chroma_mode, m_cfg.bit_depth));
    streamSize += AL_ENC_MAX_HEADER_SIZE;

    uint32_t numBufs =
        m_cfg.num_stream_bufs + static_cast<uint32_t>(kStreamSmoothingCount) + static_cast<uint32_t>(m_cfg.num_b);

    auto *pMeta = reinterpret_cast<AL_TMetaData *>(AL_StreamMetaData_Create(AL_MAX_SECTION));
    if (!pMeta)
    {
        throw std::runtime_error("Failed to create stream metadata template");
    }

    m_stream_buf_pool = std::make_unique<GenericBufPool>();
    bool ok = m_stream_buf_pool->init(m_pAllocator, numBufs, static_cast<size_t>(streamSize), pMeta, "stream_pool");
    AL_MetaData_Destroy(pMeta);

    if (!ok)
    {
        throw std::runtime_error("Failed to initialize stream buffer pool");
    }

    VIDEO_INFO_PRINT("Stream buffer pool initialized: buffer_size=%zu Bytes, num_bufs=%u",
                     m_stream_buf_pool->get_buf_size(), m_stream_buf_pool->get_num_buf());
}

void RTEncoderBase::push_stream_buffers()
{
    uint32_t total =
        m_cfg.num_stream_bufs + static_cast<uint32_t>(kStreamSmoothingCount) + static_cast<uint32_t>(m_cfg.num_b);

    for (uint32_t i = 0; i < total; i++)
    {
        auto *pBuf = m_stream_buf_pool->get_buffer(false);
        if (!pBuf)
        {
            break; // No more buffer available, push what we have
        }

        auto *pMeta = reinterpret_cast<AL_TStreamMetaData *>(AL_Buffer_GetMetaData(pBuf, AL_META_TYPE_STREAM));
        if (pMeta)
        {
            AL_StreamMetaData_ClearAllSections(pMeta);
        }

        if (!AL_Encoder_PutStreamBuffer(m_hEnc, pBuf))
        {
            m_error.store(true);
            AL_Buffer_Unref(pBuf);
            throw std::runtime_error("Failed to put stream buffer to encoder");
        }
        AL_Buffer_Unref(pBuf); // Encoder holds its own reference, safe to unref here
    }
}

// Implementation for FILE source mode
RTEncoder<SourceMode::FILE>::RTEncoder(const EncoderConfig &cfg, EncodedFrameCallback cb)
    : RTEncoderBase(cfg, std::move(cb))
{
}

AL_TBuffer *RTEncoder<SourceMode::FILE>::acquire_source_buffer()
{
    if (m_stopped.load() || m_error.load())
    {
        return nullptr;
    }

    auto pBuf = m_source_buf_pool->get_buffer();
    if (!pBuf)
    {
        return nullptr;
    }

    AL_PixMapBuffer_SetDimension(pBuf, src_resolution());

    return pBuf;
}

bool RTEncoder<SourceMode::FILE>::submit_source_buffer(AL_TBuffer *pBuf)
{
    if (!pBuf)
    {
        return false;
    }

    AL_BufferGuard buf_guard(pBuf, &AL_Buffer_Unref);

    if (!m_hEnc || m_stopped.load() || m_error.load())
    {
        return false;
    }

    if (!AL_Encoder_Process(m_hEnc, pBuf, nullptr))
    {
        m_error.store(true);
        return false;
    }
    return true;
}

void RTEncoder<SourceMode::FILE>::release_sources(AL_TBuffer const * /*pSrc*/)
{
    // File mode source buffers are owned by the encoder; nothing to do on source-release callback.
}

// Implementation for DMAFd
DMAFd::DMAFd()
    : dma_fd(-1), buffer(nullptr), width(0), height(0), fourcc(0), y_offset(0), y_pitch(0), uv_offset(0), uv_pitch(0)
{
}

DMAFd::DMAFd(int fd, AL_TBuffer *buf, uint32_t w, uint32_t h, AL_TPicFormat pic_fmt)
    : dma_fd(fd), buffer(buf), width(w), height(h), fourcc(AL_PixMapBuffer_GetFourCC(buf)), y_offset(0), y_pitch(0),
      uv_offset(0), uv_pitch(0)
{
    y_pitch = static_cast<uint32_t>(AL_PixMapBuffer_GetPlanePitch(buffer, AL_PLANE_Y));
    int strideH = AL_RoundUp(h, kHeightAlign);
    uv_offset =
        static_cast<uint32_t>(AL_GetAllocSizeSrc_PixPlane(&pic_fmt, static_cast<int>(y_pitch), strideH, AL_PLANE_Y));
    uv_pitch = static_cast<uint32_t>(AL_PixMapBuffer_GetPlanePitch(buffer, AL_PLANE_UV));
}

DMAFd::~DMAFd()
{
    if (buffer)
    {
        AL_Buffer_Unref(buffer);
        buffer = nullptr;
    }
}

DMAFd::DMAFd(DMAFd &&other) noexcept
    : dma_fd(other.dma_fd), buffer(other.buffer), width(other.width), height(other.height), fourcc(other.fourcc),
      y_offset(other.y_offset), y_pitch(other.y_pitch), uv_offset(other.uv_offset), uv_pitch(other.uv_pitch)
{
    other.dma_fd = -1;
    other.buffer = nullptr;
}

DMAFd &DMAFd::operator=(DMAFd &&other) noexcept
{
    if (this != &other)
    {
        if (buffer)
        {
            AL_Buffer_Unref(buffer);
        }

        dma_fd = other.dma_fd;
        buffer = other.buffer;
        width = other.width;
        height = other.height;
        fourcc = other.fourcc;
        y_offset = other.y_offset;
        y_pitch = other.y_pitch;
        uv_offset = other.uv_offset;
        uv_pitch = other.uv_pitch;

        other.dma_fd = -1;
        other.buffer = nullptr;
    }
    return *this;
}

// Implementation for V4L2_DMABUF source mode
RTEncoder<SourceMode::V4L2>::RTEncoder(const EncoderConfig &cfg, EncodedFrameCallback cb)
    : RTEncoderBase(cfg, std::move(cb))
{
}

RTEncoder<SourceMode::V4L2>::~RTEncoder()
{
    // Derived destructor runs before base destructor: flush here
    try
    {
        flush();
    }
    catch (...)
    {
    }

    std::lock_guard<std::mutex> lock(m_mutex);
    m_release_cb = nullptr;
}

void RTEncoder<SourceMode::V4L2>::set_release_callback(SourceReleaseCallback releaseCb)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_release_cb = std::move(releaseCb);
}

DMAFdArray RTEncoder<SourceMode::V4L2>::acquire_dma_buffers(unsigned int count)
{
    auto available_cnt = m_source_buf_pool->available_count();
    if (count > available_cnt)
    {
        VIDEO_ERROR_PRINT("Requested DMA fd count %u exceeds pool capacity %zu", count, available_cnt);
        return {};
    }

    bool ok = true;
    DMAFdArray descs;
    descs.reserve(count);

    for (unsigned int i = 0; i < count; ++i)
    {
        auto *pBuf = m_source_buf_pool->get_buffer();
        if (!pBuf)
        {
            VIDEO_ERROR_PRINT("Failed to acquire %uth source buffer from pool.", i);
            ok = false;
            break;
        }

        auto dma_fd =
            AL_LinuxDmaAllocator_GetFd(reinterpret_cast<AL_TLinuxDmaAllocator *>(m_pAllocator), pBuf->hBufs[0]);
        if (dma_fd < 0)
        {
            VIDEO_ERROR_PRINT("Buffer acquired but failed to get DMA fd for buffer %d: %s", i,
                              AL_Codec_ErrorToString(AL_Encoder_GetLastError(m_hEnc)));
            AL_Buffer_Unref(pBuf);
            ok = false;
            break;
        }

        auto dim = src_resolution();
        if (dim.iWidth <= 0 || dim.iHeight <= 0)
        {
            VIDEO_ERROR_PRINT("Invalid source resolution for DMA buffer description");
            AL_Buffer_Unref(pBuf);
            ok = false;
            break;
        }

        DMAFd desc(dma_fd, pBuf, dim.iWidth, dim.iHeight, m_pic_format);
        if (desc.y_pitch == 0 || desc.uv_pitch == 0)
        {
            VIDEO_ERROR_PRINT("Invalid plane pitch in DMA buffer description");
            ok = false;
            break;
        }

        descs.push_back(std::move(desc));
    }

    return ok ? std::move(descs) : DMAFdArray{};
}

bool RTEncoder<SourceMode::V4L2>::submit_source_buffer(AL_TBuffer *pBuf)
{
    if (m_stopped.load() || m_error.load() || !pBuf)
    {
        return false;
    }

    if (!AL_Encoder_Process(m_hEnc, pBuf, nullptr))
    {
        VIDEO_ERROR_PRINT("Failed to submit source buffer to encoder: %s",
                          AL_Codec_ErrorToString(AL_Encoder_GetLastError(m_hEnc)));
        m_error.store(true);
        return false;
    }

    return true;
}

void RTEncoder<SourceMode::V4L2>::release_sources(AL_TBuffer const *pSrc)
{
    SourceReleaseCallback release_cb;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        release_cb = m_release_cb;
    }

    try
    {
        if (release_cb)
        {
            release_cb(pSrc);
        }
    }
    catch (const std::exception &e)
    {
        m_error.store(true);
        VIDEO_ERROR_PRINT("Exception in source release callback: %s", e.what());
    }
}