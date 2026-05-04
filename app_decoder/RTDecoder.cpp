#include "RTDecoder.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

extern "C"
{
#include "lib_common/BufCommon.h"
#include "lib_common/BufferAPI.h"
#include "lib_common/BufferPictureDecMeta.h"
#include "lib_common/DisplayInfoMeta.h"
#include "lib_common/FourCC.h"
#include "lib_common/PicFormat.h"
#include "lib_common_dec/DecBuffers.h"
#include "lib_decode/DecSchedulerMcu.h"
#include "lib_fpga/DmaAlloc.h"
#include "lib_rtos/message.h"
}

namespace
{
inline int RoundUp(int value, int align)
{
    return (value + align - 1) / align * align;
}

bool IsEndOfStream(AL_TBuffer const *pFrame, AL_TInfoDecode const *pInfo)
{
    return !pFrame && !pInfo;
}

bool IsReleaseFrame(AL_TBuffer const *pFrame, AL_TInfoDecode const *pInfo)
{
    return pFrame && !pInfo;
}

bool IsMainDisplay(AL_TInfoDecode const &info)
{
    return info.eOutputID == AL_OUTPUT_MAIN || info.eOutputID == AL_OUTPUT_POSTPROC;
}
} // namespace

RTDecoder::RTDecoder(const DecoderConfig &cfg, DecodedFrameCallback cb) : m_cfg(cfg), m_frame_cb(std::move(cb))
{
    try
    {
        if (!m_frame_cb)
        {
            throw std::invalid_argument("RTDecoder: callback must not be null");
        }

        AL_DecSettings_SetDefaults(&m_dec_settings);
        m_dec_settings.eCodec = m_cfg.codec;
        m_dec_settings.eInputMode = m_cfg.input_mode;

        int invalid = AL_DecSettings_CheckValidity(&m_dec_settings, nullptr);
        if (invalid != 0)
        {
            throw std::runtime_error("RTDecoder: invalid decoder settings");
        }

        if (AL_DecSettings_CheckCoherency(&m_dec_settings, nullptr) < 0)
        {
            throw std::runtime_error("RTDecoder: fatal decoder settings incoherency");
        }

        const auto err = AL_Lib_Decoder_Init(AL_LIB_DECODER_ARCH_HOST);
        if (err != AL_SUCCESS)
        {
            throw std::runtime_error(std::string("AL_Lib_Decoder_Init failed: ") + AL_Codec_ErrorToString(err));
        }
        m_lib_initialized = true;

        m_allocator = AL_DmaAlloc_Create(m_cfg.dec_dev_path.c_str());
        if (!m_allocator)
        {
            throw std::runtime_error("RTDecoder: failed to create DMA allocator on " + m_cfg.dec_dev_path);
        }

        m_scheduler = AL_DecSchedulerMcu_Create(AL_GetHardwareDriver(), m_cfg.dec_dev_path.c_str());
        if (!m_scheduler)
        {
            throw std::runtime_error("RTDecoder: failed to create MCU decoder scheduler");
        }

        m_cbbundles.displayCB = {&RTDecoder::sdk_display, this};
        m_cbbundles.resolutionFoundCB = {&RTDecoder::sdk_resolution_found, this};
        m_cbbundles.errorCB = {&RTDecoder::sdk_error, this};

        auto create_err = AL_Decoder_Create(&m_decoder, m_scheduler, m_allocator, &m_dec_settings, &m_cbbundles);
        if (AL_IS_ERROR_CODE(create_err) || !m_decoder)
        {
            throw std::runtime_error(std::string("AL_Decoder_Create failed: ") + AL_Codec_ErrorToString(create_err));
        }

        m_input_pool = std::make_unique<BufPool>();
        if (!m_input_pool->Init(m_allocator, m_cfg.input_buffer_num, m_cfg.input_buffer_size, nullptr,
                                "rt_decoder_input"))
        {
            throw std::runtime_error("RTDecoder: failed to init input buffer pool");
        }

        m_input_pool->Commit();
    }
    catch (...)
    {
        cleanup();
        throw;
    }
}

RTDecoder::~RTDecoder()
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
    m_allow_pushback = false;
    cleanup();
}

void RTDecoder::cleanup()
{
    if (m_input_pool)
        m_input_pool->Decommit();

    if (m_decoder)
    {
        AL_Decoder_Destroy(m_decoder);
        m_decoder = nullptr;
    }

    if (m_scheduler)
    {
        AL_IDecScheduler_Destroy(m_scheduler);
        m_scheduler = nullptr;
    }

    if (m_allocator)
    {
        AL_Allocator_Destroy(m_allocator);
        m_allocator = nullptr;
    }

    if (m_lib_initialized)
    {
        AL_Lib_Decoder_DeInit();
        m_lib_initialized = false;
    }
}

void RTDecoder::signal_done()
{
    std::lock_guard<std::mutex> lock(m_eos_mutex);
    m_eos_signaled = true;
    m_eos_cv.notify_all();
}

void RTDecoder::signal_error(AL_ERR err)
{
    m_last_error = err;
    signal_done();
}

bool RTDecoder::push_stream(const void *data, size_t size, uint8_t flags)
{
    if (m_stopped.load() || AL_IS_ERROR_CODE(m_last_error.load()))
        return false;

    auto pInput = m_input_pool->GetSharedBuffer(AL_BUF_MODE_BLOCK);
    if (!pInput)
        return false; // pool was decommitted (flush was called concurrently)

    if (size > AL_Buffer_GetSize(pInput.get()))
        throw std::runtime_error("RTDecoder: push_stream data exceeds input buffer capacity");

    std::memcpy(AL_Buffer_GetData(pInput.get()), data, size);

    if (!AL_Decoder_PushStreamBuffer(m_decoder, pInput.get(), size, flags))
    {
        m_last_error = AL_Decoder_GetLastError(m_decoder);
        return false;
    }
    return true;
}

void RTDecoder::flush()
{
    if (m_stopped.exchange(true))
        return; // already flushed

    m_input_pool->Decommit();
    AL_Decoder_Flush(m_decoder);

    std::unique_lock<std::mutex> lock(m_eos_mutex);
    bool ok;
    if (m_cfg.flush_timeout_ms == 0)
    {
        m_eos_cv.wait(lock, [this] { return m_eos_signaled; });
        ok = true;
    }
    else
    {
        ok = m_eos_cv.wait_for(lock, std::chrono::milliseconds(m_cfg.flush_timeout_ms),
                               [this] { return m_eos_signaled; });
    }

    if (!ok)
    {
        m_last_error = AL_ERROR;
        throw std::runtime_error("RTDecoder: flush timeout waiting for EOS");
    }
}

int RTDecoder::decoded_frames() const
{
    return m_num_decoded.load();
}

int RTDecoder::concealed_frames() const
{
    return m_num_concealed.load();
}

AL_ERR RTDecoder::last_error() const
{
    return m_last_error.load();
}

AL_ERR RTDecoder::sdk_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                       AL_TCropInfo const *pCropInfo, void *pUserParam)
{
    auto *self = static_cast<RTDecoder *>(pUserParam);
    try
    {
        return self->on_resolution_found(iBufferNumber, pStreamSettings, pCropInfo);
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
        self->on_display(pFrame, pInfo);
    }
    catch (...)
    {
        self->signal_error(AL_ERROR);
    }
}

void RTDecoder::sdk_error(AL_ERR eError, void *pUserParam)
{
    auto *self = static_cast<RTDecoder *>(pUserParam);
    self->on_error(eError);
}

AL_ERR RTDecoder::on_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                      AL_TCropInfo const *pCropInfo)
{
    (void)pCropInfo;

    // Step 1: Derive a fully-resolved output format from stream properties.
    // Every field is determined by the stream; RTDecoder has no user-configurable overrides.
    AL_TDecOutputSettings output_settings = derive_output_settings(*pStreamSettings);

    // Step 2: Tell the decoder what output format we want.
    if (!AL_Decoder_ConfigureOutputSettings(m_decoder, &output_settings))
    {
        return AL_ERR_REQUEST_MALFORMED;
    }

    // Step 3: Compute stride-aligned buffer dimensions.
    // Hardware requires width/height multiples of 64 (covers both H.264 MB and H.265 LCU).
    AL_TDimension out_dim = pStreamSettings->tDim;
    out_dim.iWidth = RoundUp(out_dim.iWidth, 64);
    out_dim.iHeight = RoundUp(out_dim.iHeight, 64);
    const int min_pitch = AL_Decoder_GetMinPitch(out_dim.iWidth, &output_settings.tPicFormat);

    // Step 4: If the rec pool was already created, check whether the existing buffers
    // can accommodate the new format.  Buffers are reusable as long as the new layout
    // fits within the space that was originally allocated.
    if (m_rec_pool)
    {
        return can_reuse_rec_pool(output_settings.tPicFormat, out_dim, min_pitch) ? AL_SUCCESS : AL_ERR_NO_MEMORY;
    }

    // Step 5: First-time pool creation.
    // Describe plane layout to PixMapBufPool, then allocate and hand every buffer to
    // the decoder so it can immediately start filling them.
    configure_rec_pool(output_settings.tPicFormat, out_dim, min_pitch);

    const int num_buf = iBufferNumber + 1; // one extra buffer held by display consumer
    if (!m_rec_pool->Init(m_allocator, num_buf, "rt_decoder_rec"))
    {
        return AL_ERR_NO_MEMORY;
    }

    for (int i = 0; i < num_buf; ++i)
    {
        auto pDecPict = m_rec_pool->GetSharedBuffer(AL_BUF_MODE_NONBLOCK);
        if (!pDecPict)
        {
            return AL_ERR_NO_MEMORY;
        }

        AL_Buffer_Cleanup(pDecPict.get());

        if (!attach_display_metadata(pDecPict.get()))
        {
            return AL_ERR_NO_MEMORY;
        }

        if (!AL_Decoder_PutDisplayPicture(m_decoder, pDecPict.get()))
        {
            return AL_ERR_REQUEST_MALFORMED;
        }
    }

    // Step 6: Record what we allocated so future resolution-found events can
    // compare against it (Step 4 above).  m_rec_pool being non-null signals
    // that the pool is initialized (replaces the old 'initialized' bool).
    m_rec_pool_alloc.dim = out_dim;
    m_rec_pool_alloc.pitch_y = min_pitch;
    m_rec_pool_alloc.pic_format = output_settings.tPicFormat;
    return AL_SUCCESS;
}

void RTDecoder::on_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo)
{
    if (IsEndOfStream(pFrame, pInfo))
    {
        signal_done();
        return;
    }

    // Release-frame notification: decoder is reclaiming a buffer it no longer needs.
    // No action required from us.
    if (IsReleaseFrame(pFrame, pInfo))
        return;

    // From here both pFrame and pInfo are valid (normal display callback).
    AL_Buffer_InvalidateMemory(pFrame);

    const AL_ERR frame_err = AL_Decoder_GetFrameError(m_decoder, pFrame);
    if (AL_IS_ERROR_CODE(frame_err))
    {
        signal_error(frame_err);
        return;
    }

    if (frame_err == AL_WARN_CONCEAL_DETECT || frame_err == AL_WARN_HW_CONCEAL_DETECT ||
        frame_err == AL_WARN_INVALID_ACCESS_UNIT_STRUCTURE)
    {
        ++m_num_concealed;
    }

    if (!IsMainDisplay(*pInfo))
        return;

    ++m_num_decoded;

    // Hold an extra reference across the user callback so the buffer cannot be
    // freed underneath us if the decoder recycles it before the callback returns.
    AL_Buffer_Ref(pFrame);
    try
    {
        m_frame_cb(pFrame, *pInfo);
    }
    catch (...)
    {
        AL_Buffer_Unref(pFrame);
        signal_error(AL_ERROR);
        return;
    }
    AL_Buffer_Unref(pFrame);

    // Return the buffer to the decoder pool so it can be reused for future frames.
    if (m_allow_pushback.load(std::memory_order_relaxed))
    {
        if (!AL_Decoder_PutDisplayPicture(m_decoder, pFrame))
            signal_error(AL_ERROR);
    }
}

void RTDecoder::on_error(AL_ERR eError)
{
    m_last_error = eError;

    if (AL_IS_ERROR_CODE(eError))
    {
        m_allow_pushback = false;
        signal_done();
    }
}

AL_TDecOutputSettings RTDecoder::derive_output_settings(AL_TStreamSettings const &stream_settings)
{
    AL_TDecOutputSettings s;
    SetDefaultDecOutputSettings(&s);
    AL_TPicFormat &fmt = s.tPicFormat;

    // Chroma mode and bit depth come directly from the stream.
    fmt.eChromaMode = stream_settings.eChroma;
    fmt.uBitDepth = static_cast<uint8_t>(stream_settings.iBitDepth);

    // Plane mode follows chroma mode.
    fmt.ePlaneMode = GetInternalBufPlaneMode(fmt.eChromaMode);

    // 10-bit raster output uses XV20/XV15 packed format.
    if (fmt.eStorageMode == AL_FB_RASTER && fmt.uBitDepth == 10)
        fmt.eSamplePackMode = AL_SAMPLE_PACK_MODE_PACKED_XV;

    return s;
}

bool RTDecoder::can_reuse_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y) const
{
    // Format must be byte-for-byte compatible: any layout change means the plane
    // offsets / strides change and the existing buffers cannot be reused.
    auto const &a = m_rec_pool_alloc.pic_format;
    if (pic_format.eChromaMode != a.eChromaMode || pic_format.uBitDepth != a.uBitDepth ||
        pic_format.eStorageMode != a.eStorageMode || pic_format.bCompressed != a.bCompressed ||
        pic_format.ePlaneMode != a.ePlaneMode || pic_format.eComponentOrder != a.eComponentOrder ||
        pic_format.eSamplePackMode != a.eSamplePackMode)
    {
        return false;
    }

    // Buffer dimensions and pitch must fit within what was originally allocated.
    return pitch_y <= m_rec_pool_alloc.pitch_y && dim.iWidth <= m_rec_pool_alloc.dim.iWidth &&
           dim.iHeight <= m_rec_pool_alloc.dim.iHeight;
}

void RTDecoder::configure_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y)
{
    m_rec_pool = std::make_unique<PixMapBufPool>();

    const auto fourcc = AL_GetFourCC(pic_format);
    m_rec_pool->SetFormat(dim, fourcc);

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

    m_rec_pool->AddChunk(static_cast<size_t>(offset), plane_descs);
}

bool RTDecoder::attach_display_metadata(AL_TBuffer *pDecPict)
{
    auto *pPictureDecMeta = AL_PictureDecMetaData_Create();
    if (!pPictureDecMeta || !AL_Buffer_AddMetaData(pDecPict, reinterpret_cast<AL_TMetaData *>(pPictureDecMeta)))
    {
        if (pPictureDecMeta)
        {
            AL_MetaData_Destroy(reinterpret_cast<AL_TMetaData *>(pPictureDecMeta));
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

// ---------------------------------------------------------------------------
// Convenience free function: decode a complete bitstream file
// ---------------------------------------------------------------------------

void decode_file(RTDecoder &decoder, const std::string &bitstream_path, const std::string &split_sizes_path)
{
    const DecoderConfig &cfg = decoder.config();

    std::ifstream input(bitstream_path, std::ios::binary);
    if (!input.is_open())
        throw std::runtime_error("decode_file: cannot open bitstream: " + bitstream_path);

    std::vector<char> buf(cfg.input_buffer_size);

    if (cfg.input_mode == AL_DEC_SPLIT_INPUT)
    {
        if (split_sizes_path.empty())
            throw std::runtime_error("decode_file: split input mode requires a sizes file");

        std::ifstream sizes(split_sizes_path);
        if (!sizes.is_open())
            throw std::runtime_error("decode_file: cannot open split sizes file: " + split_sizes_path);

        std::string line;
        while (std::getline(sizes, line))
        {
            if (line.empty())
                continue;

            const int frame_size = std::stoi(line);
            if (frame_size <= 0)
                continue;

            if (static_cast<size_t>(frame_size) > cfg.input_buffer_size)
                throw std::runtime_error("decode_file: split frame size exceeds input buffer capacity");

            input.read(buf.data(), frame_size);
            if (input.gcount() != frame_size)
                throw std::runtime_error("decode_file: bitstream shorter than split size entry");

            const uint8_t flags = AL_STREAM_BUF_FLAG_ENDOFSLICE | AL_STREAM_BUF_FLAG_ENDOFFRAME;
            if (!decoder.push_stream(buf.data(), static_cast<size_t>(frame_size), flags))
                break;
        }
    }
    else
    {
        while (true)
        {
            input.read(buf.data(), static_cast<std::streamsize>(cfg.input_buffer_size));
            const size_t n = static_cast<size_t>(input.gcount());
            if (n == 0)
                break;
            if (!decoder.push_stream(buf.data(), n))
                break;
        }
    }

    decoder.flush();
}
