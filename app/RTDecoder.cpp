#include "RTDecoder.h"

extern "C"
{
#include "lib_common/BufCommon.h"
#include "lib_common/BufferAPI.h"
#include "lib_common/BufferPictureDecMeta.h"
#include "lib_common/DisplayInfoMeta.h"
#include "lib_common/FourCC.h"
#include "lib_common/PicFormat.h"
#include "lib_common/Utils.h"
#include "lib_common_dec/DecBuffers.h"
#include "lib_decode/DecSchedulerMcu.h"
#include "lib_fpga/DmaAlloc.h"
#include "lib_rtos/message.h"
}

#include <cstring>

using AL_BufferGuard = std::unique_ptr<AL_TBuffer, decltype(&AL_Buffer_Unref)>;

RTDecoder::RTDecoder(const DecoderConfig &cfg, DecodedFrameCallback cb)
    : m_cfg(cfg), m_callback(std::move(cb)), m_pAllocator(nullptr), m_pScheduler(nullptr), m_hDec(nullptr),
      m_cbbundles{}, m_state{State::Running}, m_fps(0.0), m_frame_count(0),
      m_fps_last_time(std::chrono::steady_clock::now()), m_lib_initialized(false)
{
    try
    {
        if (!m_callback)
        {
            throw std::invalid_argument("DecodedFrameCallback must not be null");
        }

        auto err = AL_Lib_Decoder_Init(AL_LIB_DECODER_ARCH_HOST);
        if (err != AL_SUCCESS)
        {
            throw std::runtime_error(std::string("AL_Lib_Decoder_Init failed: ") + AL_Codec_ErrorToString(err));
        }
        m_lib_initialized = true;

        m_pAllocator = AL_DmaAlloc_Create(m_cfg.dec_dev_path.c_str());
        if (!m_pAllocator)
        {
            throw std::runtime_error("Failed to create DMA allocator for device: " + m_cfg.dec_dev_path);
        }

        m_pScheduler = AL_DecSchedulerMcu_Create(AL_GetHardwareDriver(), m_cfg.dec_dev_path.c_str());
        if (!m_pScheduler)
        {
            throw std::runtime_error("Failed to create MCU decoder scheduler");
        }

        AL_DecSettings_SetDefaults(&m_dec_settings);
        m_dec_settings.eCodec = m_cfg.codec;
        m_dec_settings.eInputMode = m_cfg.input_mode;
        auto result = AL_DecSettings_CheckValidity(&m_dec_settings, nullptr);
        if (result > 0)
        {
            throw std::runtime_error("AL_DecSettings_CheckValidity found " + std::to_string(result) +
                                     " invalid parameter(s)");
        }

        if (AL_DecSettings_CheckCoherency(&m_dec_settings, nullptr) < 0)
        {
            throw std::runtime_error("RTDecoder: fatal decoder settings incoherency");
        }

        m_cbbundles.displayCB = {&RTDecoder::sdk_display, this};
        m_cbbundles.resolutionFoundCB = {&RTDecoder::sdk_resolution_found, this};
        m_cbbundles.errorCB = {&RTDecoder::sdk_error, this};

        err = AL_Decoder_Create(&m_hDec, m_pScheduler, m_pAllocator, &m_dec_settings, &m_cbbundles);
        if (AL_IS_ERROR_CODE(err) || !m_hDec)
        {
            throw std::runtime_error(std::string("AL_Decoder_Create failed: ") + AL_Codec_ErrorToString(err));
        }

        m_src_buf_pool = std::make_unique<GenericBufPool>();
        if (!m_src_buf_pool->init(m_pAllocator, m_cfg.input_buffer_num, m_cfg.input_buffer_size, nullptr,
                                  "rt_decoder_stream"))
        {
            throw std::runtime_error("Failed to initialize stream buffer pool");
        }
    }
    catch (...)
    {
        cleanup();
        throw;
    }
}

RTDecoder::~RTDecoder()
{
    if (m_state.load() == State::Running)
    {
        try
        {
            flush();
        }
        catch (...)
        {
            // ignore flush errors in destructor
        }
    }

    m_state.store(State::Stopping, std::memory_order_release);
    cleanup();
}

bool RTDecoder::push_stream(const void *data, size_t size, uint8_t flags)
{
    if (m_state.load() != State::Running)
    {
        return false;
    }

    auto buf = m_src_buf_pool->get_buffer();
    if (!buf)
    {
        return false;
    }

    AL_BufferGuard buf_guard(buf, &AL_Buffer_Unref);
    if (size > AL_Buffer_GetSize(buf))
    {
        VIDEO_ERROR_PRINT("Input stream size %zu exceeds buffer capacity %zu", size, AL_Buffer_GetSize(buf));
        return false;
    }

    std::memcpy(AL_Buffer_GetData(buf), data, size);
    if (!AL_Decoder_PushStreamBuffer(m_hDec, buf, size, flags))
    {
        signal_error(AL_ERROR);
        return false;
    }

    return true;
}

void RTDecoder::flush()
{
    State expected = State::Running;
    if (!m_state.compare_exchange_strong(expected, State::Flushing))
    {
        return; // already Flushing, Done, or Stopping
    }

    m_src_buf_pool->decommit();
    AL_Decoder_Flush(m_hDec);

    auto is_done = [this] { return m_state.load(std::memory_order_relaxed) >= State::Done; };

    std::unique_lock<std::mutex> lock(m_eos_mutex);
    bool ok;
    if (m_cfg.flush_timeout_ms == 0)
    {
        m_eos_cv.wait(lock, is_done);
        ok = true;
    }
    else
    {
        ok = m_eos_cv.wait_for(lock, std::chrono::milliseconds(m_cfg.flush_timeout_ms), is_done);
    }

    if (!ok)
    {
        m_state.store(State::Done); // force terminal even on timeout
        throw std::runtime_error("RTDecoder: flush timeout waiting for EOS");
    }
}

AL_ERR RTDecoder::sdk_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                       AL_TCropInfo const *pCropInfo, void *pUserParam)
{
    auto *self = static_cast<RTDecoder *>(pUserParam);
    try
    {
        return self->on_sdk_resolution_found(iBufferNumber, pStreamSettings, pCropInfo);
    }
    catch (...)
    {
        self->signal_error(AL_ERROR);
        return AL_ERROR;
    }
}

void RTDecoder::sdk_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo, void *pUserParam)
{
    auto *self = static_cast<RTDecoder *>(pUserParam);
    try
    {
        self->on_sdk_display(pFrame, pInfo);
    }
    catch (...)
    {
        self->signal_error(AL_ERROR);
    }
}

void RTDecoder::sdk_error(AL_ERR eError, void *pUserParam)
{
    auto *self = static_cast<RTDecoder *>(pUserParam);
    // Warnings (e.g. AL_WARN_CONCEAL_DETECT) do not stop the pipeline;
    // they are already surfaced per-frame via AL_Decoder_GetFrameError.
    if (AL_IS_ERROR_CODE(eError))
    {
        self->signal_error(eError);
    }
}

AL_ERR RTDecoder::on_sdk_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                          AL_TCropInfo const *pCropInfo)
{
    (void)pCropInfo;
    AL_TDecOutputSettings output_settings = derive_output_settings(*pStreamSettings);
    if (!AL_Decoder_ConfigureOutputSettings(m_hDec, &output_settings))
    {
        return AL_ERR_REQUEST_MALFORMED;
    }
    AL_TDimension out_dim = pStreamSettings->tDim;
    out_dim.iWidth = RoundUp(out_dim.iWidth, 64);
    out_dim.iHeight = RoundUp(out_dim.iHeight, 64);
    const int min_pitch = AL_Decoder_GetMinPitch(out_dim.iWidth, &output_settings.tPicFormat);

    if (m_rec_buf_pool)
    {
        return can_reuse_rec_pool(output_settings.tPicFormat, out_dim, min_pitch) ? AL_SUCCESS : AL_ERR_NO_MEMORY;
    }

    configure_rec_pool(output_settings.tPicFormat, out_dim, min_pitch);

    const int num_buf = iBufferNumber + 1; // one extra buffer held by display consumer
    if (!m_rec_buf_pool->init(m_pAllocator, num_buf, "rt_decoder_rec"))
    {
        return AL_ERR_NO_MEMORY;
    }

    for (int i = 0; i < num_buf; ++i)
    {
        auto pDecPict = m_rec_buf_pool->get_buffer(false);
        AL_BufferGuard dec_pict_guard(pDecPict, &AL_Buffer_Unref);
        if (!pDecPict)
        {
            return AL_ERR_NO_MEMORY;
        }

        AL_Buffer_Cleanup(pDecPict);

        if (!attach_display_metadata(pDecPict))
        {
            return AL_ERR_NO_MEMORY;
        }

        if (!AL_Decoder_PutDisplayPicture(m_hDec, pDecPict))
        {
            return AL_ERR_REQUEST_MALFORMED;
        }
    }

    m_rec_pool_alloc.dim = out_dim;
    m_rec_pool_alloc.pitch_y = min_pitch;
    m_rec_pool_alloc.pic_format = output_settings.tPicFormat;
    return AL_SUCCESS;
}

void RTDecoder::on_sdk_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo)
{
    // end of stream signal (no more frames to decode)
    if (!pFrame && !pInfo)
    {
        signal_done();
        return;
    }

    if (pFrame && !pInfo)
    {
        return;
    }

    AL_Buffer_Ref(pFrame);
    AL_BufferGuard frame_guard(pFrame, &AL_Buffer_Unref);
    AL_Buffer_InvalidateMemory(pFrame);

    auto fr_err = AL_Decoder_GetFrameError(m_hDec, pFrame);
    if (AL_IS_ERROR_CODE(fr_err))
    {
        signal_error(fr_err);
        return;
    }
    // Concealment warnings (AL_WARN_CONCEAL_DETECT, AL_WARN_HW_CONCEAL_DETECT,
    // AL_WARN_INVALID_ACCESS_UNIT_STRUCTURE) do NOT abort processing.
    // The frame is still a valid displayable picture (with concealment artifacts).
    // Falling through ensures the buffer is delivered to the callback and then
    // returned to the decoder via PutDisplayPicture, preventing pool depletion.
    // m_num_concealed += (fr_err != AL_SUCCESS);

    bool is_main = pInfo->eOutputID == AL_OUTPUT_MAIN || pInfo->eOutputID == AL_OUTPUT_POSTPROC;

    if (!is_main)
    {
        return;
    }

    // m_num_decoded++;

    // Call user callback with decoded frame
    try
    {
        m_callback(pFrame, *pInfo);
    }
    catch (...)
    {
        signal_error(AL_ERROR);
        return;
    }

    update_fps();

    // Only return the frame to the decoder while the pipeline is alive.
    // During normal flush (Flushing state) we continue returning buffers
    // so the hardware pipeline keeps draining.
    auto s = m_state.load(std::memory_order_relaxed);
    if (s == State::Running || s == State::Flushing)
    {
        if (!AL_Decoder_PutDisplayPicture(m_hDec, pFrame))
        {
            signal_error(AL_ERROR);
        }
    }
}

void RTDecoder::on_sdk_error(AL_ERR eError)
{
    if (AL_IS_ERROR_CODE(eError))
    {
        // Fatal: stop the pipeline and wake any waiter in flush().
        signal_error(eError);
    }
    else if (AL_IS_WARNING_CODE(eError))
    {
        VIDEO_DEBUG_PRINT("Decoder warning: %s", AL_Codec_ErrorToString(eError));
    }
}

AL_TDecOutputSettings RTDecoder::derive_output_settings(AL_TStreamSettings const &stream_settings)
{
    AL_TDecOutputSettings s;
    SetDefaultDecOutputSettings(&s);
    AL_TPicFormat &fmt = s.tPicFormat;

    fmt.eChromaMode = stream_settings.eChroma;
    fmt.uBitDepth = static_cast<uint8_t>(stream_settings.iBitDepth);
    fmt.ePlaneMode = GetInternalBufPlaneMode(fmt.eChromaMode);
    if (fmt.eStorageMode == AL_FB_RASTER && fmt.uBitDepth == 10)
    {
        fmt.eSamplePackMode = AL_SAMPLE_PACK_MODE_PACKED_XV;
    }

    return s;
}

void RTDecoder::configure_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y)
{
    const auto fourcc = AL_GetFourCC(pic_format);
    m_rec_buf_pool = std::make_unique<PixMapBufPool>();
    m_rec_buf_pool->set_format(dim, fourcc);

    std::vector<AL_TPlaneDescription> plane_descs;
    AL_EPlaneId used_planes[AL_MAX_BUFFER_PLANES];
    const int num_planes = AL_Plane_GetBufferPixelPlanes(pic_format, used_planes);

    int offset = 0;
    for (int i = 0; i < num_planes; ++i)
    {
        int pitch = (used_planes[i] == AL_PLANE_Y || used_planes[i] == AL_PLANE_YUV)
                        ? pitch_y
                        : AL_GetChromaPitch(fourcc, pitch_y);

        plane_descs.push_back(AL_TPlaneDescription{used_planes[i], offset, pitch});
        offset += AL_DecGetAllocSize_Frame_PixPlane(&pic_format, dim, pitch, used_planes[i]);
    }

    m_rec_buf_pool->add_chunk(static_cast<size_t>(offset), plane_descs);
}

bool RTDecoder::attach_display_metadata(AL_TBuffer *pDecPict)
{
    auto *pMeta = AL_PictureDecMetaData_Create();
    if (!pMeta || !AL_Buffer_AddMetaData(pDecPict, reinterpret_cast<AL_TMetaData *>(pMeta)))
    {
        if (pMeta)
        {
            AL_MetaData_Destroy(reinterpret_cast<AL_TMetaData *>(pMeta));
        }
        return false;
    }

    auto *pDisplayInfoMeta = AL_DisplayInfoMetaData_Create();
    if (!pDisplayInfoMeta || !AL_Buffer_AddMetaData(pDecPict, reinterpret_cast<AL_TMetaData *>(pDisplayInfoMeta)))
    {
        if (pDisplayInfoMeta)
        {
            AL_MetaData_Destroy(reinterpret_cast<AL_TMetaData *>(pDisplayInfoMeta));
        }
        return false;
    }

    return true;
}

bool RTDecoder::can_reuse_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y) const
{
    auto const &a = m_rec_pool_alloc.pic_format;
    if (pic_format.eChromaMode != a.eChromaMode || pic_format.uBitDepth != a.uBitDepth ||
        pic_format.eStorageMode != a.eStorageMode || pic_format.bCompressed != a.bCompressed ||
        pic_format.ePlaneMode != a.ePlaneMode || pic_format.eComponentOrder != a.eComponentOrder ||
        pic_format.eSamplePackMode != a.eSamplePackMode)
    {
        return false;
    }

    return pitch_y <= m_rec_pool_alloc.pitch_y && dim.iWidth <= m_rec_pool_alloc.dim.iWidth &&
           dim.iHeight <= m_rec_pool_alloc.dim.iHeight;
}

void RTDecoder::signal_done()
{
    {
        std::lock_guard<std::mutex> lock(m_eos_mutex);
        // Only advance to Done from an active state; never overwrite Stopping
        // (set by the destructor) as that would allow PutDisplayPicture again.
        State cur = m_state.load(std::memory_order_relaxed);
        if (cur == State::Running || cur == State::Flushing)
        {
            m_state.store(State::Done, std::memory_order_relaxed);
        }
    }
    m_eos_cv.notify_all();
}

void RTDecoder::signal_error(AL_ERR err)
{
    VIDEO_ERROR_PRINT("Decoder error: %s", AL_Codec_ErrorToString(err));
    signal_done(); // Done state covers both clean EOS and fatal error
}

double RTDecoder::fps() const
{
    std::lock_guard<std::mutex> lock(m_fps_mutex);
    return m_fps;
}

void RTDecoder::update_fps()
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
    {
        std::lock_guard<std::mutex> lock(m_fps_mutex);
        m_fps = 0.1 * m_fps + 0.9 * fps;
    }
    m_fps_last_time = now;
    m_frame_count = 0;
}

void RTDecoder::cleanup()
{
    if (m_src_buf_pool)
    {
        m_src_buf_pool->decommit();
    }

    if (m_hDec)
    {
        AL_Decoder_Destroy(m_hDec);
        m_hDec = nullptr;
    }

    if (m_pScheduler)
    {
        AL_IDecScheduler_Destroy(m_pScheduler);
        m_pScheduler = nullptr;
    }

    if (m_pAllocator)
    {
        AL_Allocator_Destroy(m_pAllocator);
        m_pAllocator = nullptr;
    }

    if (m_lib_initialized)
    {
        AL_Lib_Decoder_DeInit();
        m_lib_initialized = false;
    }
}