#include "RTEncoder.h"
#include <algorithm>

extern "C"
{
#include "lib_common/BufferAPI.h"
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

static constexpr int kStreamSmoothingCount = 2;
static constexpr int kHeightAlign = 8;
static constexpr uint32_t kMaxWidth = 3840;
static constexpr uint32_t kMaxHeight = 2160;

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

static uint32_t reconstruct_stream(DMAProxy &mem_ctl, AL_TBuffer *stream, int firstSection)
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
    }

    return size;
}

RTEncoderBase::RTEncoderBase(const EncoderConfig &cfg, EncodedFrameCallback cb)
    : m_cfg(cfg), m_callback(std::move(cb)), m_dma_proxy(cfg.dma_dev_path.c_str()), m_pAllocator(nullptr),
      m_pScheduler(nullptr), m_pic_format{}, m_src_fourcc{}, m_stopped(false), m_eos_signaled(false), m_error(false)
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

    m_pAllocator = AL_DmaAlloc_Create(m_cfg.enc_dev_path.c_str());
    if (!m_pAllocator)
    {
        AL_Lib_Encoder_DeInit();
        throw std::runtime_error("Failed to create DMA allocator for device: " + m_cfg.enc_dev_path);
    }

    m_pScheduler = AL_SchedulerMcu_Create(
        AL_GetHardwareDriver(), reinterpret_cast<AL_TLinuxDmaAllocator *>(m_pAllocator), m_cfg.enc_dev_path.c_str());
    if (!m_pScheduler)
    {
        AL_Allocator_Destroy(m_pAllocator);
        m_pAllocator = nullptr;
        AL_Lib_Encoder_DeInit();
        throw std::runtime_error("Failed to create MCU scheduler");
    }

    m_pic_format = AL_EncGetSrcPicFormat(m_cfg.chroma_mode, m_cfg.bit_depth, AL_SRC_RASTER);
    m_src_fourcc = AL_EncGetSrcFourCC(m_pic_format);

    AL_TEncSettings settings{};
    AL_Settings_SetDefaults(&settings);
    AL_Settings_SetDefaultParam(&settings);
    auto configed_width = m_cfg.width;
    auto configed_height = m_cfg.height;
    m_cfg.width = kMaxWidth;
    m_cfg.height = kMaxHeight;
    init_settings(settings);

    auto result = AL_Settings_CheckValidity(&settings, &settings.tChParam[0], nullptr);
    if (result > 0)
    {
        throw std::runtime_error("AL_Settings_CheckValidity found " + std::to_string(result) + " invalid parameter(s)");
    }

    result = AL_Settings_CheckCoherency(&settings, &settings.tChParam[0], m_src_fourcc, nullptr);
    if (result < 0)
    {
        throw std::runtime_error("AL_Settings_CheckCoherency: fatal incoherency");
    }

    err = AL_Encoder_Create(&m_hEnc, m_pScheduler, m_pAllocator, &settings, {&RTEncoderBase::sdk_callback, this});
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

    init_stream_buf_pool();
    push_stream_buffers();
    set_resolution(configed_width, configed_height);
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

    AL_Lib_Encoder_DeInit();
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

TFourCC RTEncoderBase::SRC_FourCC() const
{
    return m_src_fourcc;
}

uint8_t RTEncoderBase::SRC_bitdepth() const
{
    return m_cfg.bit_depth;
}

AL_EChromaMode RTEncoderBase::SRC_chroma() const
{
    return m_cfg.chroma_mode;
}

AL_TDimension RTEncoderBase::SRC_resolution() const
{
    std::lock_guard<std::mutex> lock(m_cfg_mutex);
    return AL_TDimension{m_cfg.width, m_cfg.height};
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
    if (pMeta)
    {
        if (pMeta->uNumSection > 0)
        {
            const auto *pBase = AL_Buffer_GetData(pStream);
            const auto uSize = reconstruct_stream(m_dma_proxy, pStream, 0);

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
}

void RTEncoderBase::init_settings(AL_TEncSettings &settings) const
{
    AL_TEncChanParam &ch = settings.tChParam[0];

    ch.uEncWidth = m_cfg.width;
    ch.uEncHeight = m_cfg.height;
    ch.uSrcWidth = m_cfg.width;
    ch.uSrcHeight = m_cfg.height;

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
        // ch.uNumSlices = 8;
        // ch.uSliceSize = 0;
        // ch.eEncOptions = static_cast<AL_EChEncOption>(ch.eEncOptions | AL_OPT_LOWLAT_INT);
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

void RTEncoderBase::init_stream_buf_pool()
{
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

RTEncoder<SourceMode::FILE>::RTEncoder(const EncoderConfig &cfg, EncodedFrameCallback cb)
    : RTEncoderBase(cfg, std::move(cb))
{
    init_source_buf_pool();
}

AL_TBuffer *RTEncoder<SourceMode::FILE>::acquire_source_buffer()
{
    if (m_stopped.load() || m_error.load())
    {
        return nullptr;
    }

    auto pBuf = m_source_buf_pool->get_buffer();
    if (pBuf)
    {
        AL_PixMapBuffer_SetDimension(pBuf, SRC_resolution());
    }

    return pBuf;
}

bool RTEncoder<SourceMode::FILE>::submit_source_buffer(AL_TBuffer *pBuf)
{
    if (!pBuf)
    {
        return false;
    }

    std::unique_ptr<AL_TBuffer, decltype(&AL_Buffer_Unref)> buf_guard(pBuf, &AL_Buffer_Unref);

    if (m_stopped.load() || m_error.load())
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

void RTEncoder<SourceMode::FILE>::init_source_buf_pool()
{
    AL_TDimension tDim{static_cast<int32_t>(m_cfg.width), static_cast<int32_t>(m_cfg.height)};
    AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES]{};
    int nPlanes = AL_Plane_GetBufferPixelPlanes(m_pic_format, usedPlanes);

    std::vector<AL_TPlaneDescription> plane_descs;
    int chunk_size = 0;

    int pitchY = AL_EncGetMinPitch(m_cfg.width, &m_pic_format);
    int strideH = AL_RoundUp(m_cfg.height, kHeightAlign);
    for (int i = 0; i < nPlanes; i++)
    {
        auto pitch = (usedPlanes[i] == AL_PLANE_Y) ? pitchY : AL_GetChromaPitch(m_src_fourcc, pitchY);
        plane_descs.push_back({usedPlanes[i], chunk_size, pitch});
        chunk_size += static_cast<int>(AL_GetAllocSizeSrc_PixPlane(&m_pic_format, pitchY, strideH, usedPlanes[i]));
    }

    m_source_buf_pool = std::make_unique<PixMapBufPool>();
    m_source_buf_pool->set_format(tDim, m_src_fourcc);
    m_source_buf_pool->add_chunk(chunk_size, plane_descs);

    if (!m_source_buf_pool->init(m_pAllocator, m_cfg.num_src_bufs, "source_pool"))
    {
        throw std::runtime_error("Failed to initialize source buffer pool");
    }

    VIDEO_INFO_PRINT("Source buffer pool initialized: format=%s, size=%ux%u, planes=%d, chunk_size=%d, num_bufs=%u",
                     FOURCC2STR(m_src_fourcc).c_str(), tDim.iWidth, tDim.iHeight, nPlanes, chunk_size,
                     m_cfg.num_src_bufs);
}

void RTEncoder<SourceMode::FILE>::release_sources(AL_TBuffer const * /*pSrc*/)
{
    // for file mode, source buffer is owned by the encoder and need do nothing on release callback
}

RTEncoder<SourceMode::V4L2>::RTEncoder(const EncoderConfig &cfg, EncodedFrameCallback cb)
    : RTEncoderBase(cfg, std::move(cb))
{
}

void RTEncoder<SourceMode::V4L2>::set_release_callback(SourceReleaseCallback releaseCb, void *userData)
{
    std::lock_guard<std::mutex> lock(m_fd_mutex);
    m_release_cb = std::move(releaseCb);
    m_usr_data = userData;
}

bool RTEncoder<SourceMode::V4L2>::submit_dma_fd(int fd, size_t data_size)
{
    if (m_stopped.load() || m_error.load())
    {
        return false;
    }

    if (fd < 0 || data_size == 0)
    {
        return false;
    }

    if (m_stopped.load() || m_error.load())
    {
        return false;
    }

    auto *pLinuxAllocator = reinterpret_cast<AL_TLinuxDmaAllocator *>(m_pAllocator);
    AL_HANDLE dmaHandle = AL_LinuxDmaAllocator_ImportFromFd(pLinuxAllocator, fd);
    if (!dmaHandle)
    {
        return false;
    }

    auto src_dim = SRC_resolution();
    AL_TBuffer *pSrcBuf = AL_PixMapBuffer_Create(m_pAllocator, AL_Buffer_Destroy, src_dim, m_src_fourcc);
    if (!pSrcBuf)
    {
        AL_Allocator_Free(m_pAllocator, dmaHandle);
        return false;
    }

    AL_EPlaneId usedPlanes[AL_MAX_BUFFER_PLANES]{};
    int nPlanes = AL_Plane_GetBufferPixelPlanes(m_pic_format, usedPlanes);

    std::vector<AL_TPlaneDescription> planeDescs;
    planeDescs.reserve(static_cast<size_t>(nPlanes));

    int pitchY = AL_EncGetMinPitch(src_dim.iWidth, &m_pic_format);
    int strideH = AL_RoundUp(src_dim.iHeight, kHeightAlign);
    int offset = 0;
    for (int i = 0; i < nPlanes; ++i)
    {
        int pitch = (usedPlanes[i] == AL_PLANE_Y) ? pitchY : AL_GetChromaPitch(m_src_fourcc, pitchY);
        planeDescs.push_back(AL_TPlaneDescription{usedPlanes[i], offset, pitch});
        offset += static_cast<int>(AL_GetAllocSizeSrc_PixPlane(&m_pic_format, pitchY, strideH, usedPlanes[i]));
    }

    if (!AL_PixMapBuffer_AddPlanes(pSrcBuf, dmaHandle, data_size, planeDescs.data(), nPlanes))
    {
        AL_Buffer_Unref(pSrcBuf);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_fd_mutex);
        m_fd_map[pSrcBuf] = fd;
    }

    if (!AL_Encoder_Process(m_hEnc, pSrcBuf, nullptr))
    {
        {
            std::lock_guard<std::mutex> lock(m_fd_mutex);
            m_fd_map.erase(pSrcBuf);
        }
        AL_Buffer_Unref(pSrcBuf);
        VIDEO_ERROR_PRINT("Failed to submit dmabuf fd %d to encoder", fd);
        m_error.store(true);
        return false;
    }

    AL_Buffer_Unref(pSrcBuf);
    return true;
}

void RTEncoder<SourceMode::V4L2>::release_sources(AL_TBuffer const *pSrc)
{
    int fd = -1;
    SourceReleaseCallback release_cb;
    {
        std::lock_guard<std::mutex> lock(m_fd_mutex);
        auto it = m_fd_map.find(pSrc);
        if (it != m_fd_map.end())
        {
            fd = it->second;
            m_fd_map.erase(it);
        }
        release_cb = m_release_cb;
    }

    if (fd < 0)
    {
        VIDEO_ERROR_PRINT("Failed to find fd for released source buffer");
        m_error.store(true);
    }

    try
    {
        if (release_cb)
        {
            release_cb(fd, m_usr_data);
        }
    }
    catch (const std::exception &e)
    {
        m_error.store(true);
        VIDEO_ERROR_PRINT("Exception in source release callback: %s", e.what());
    }
}