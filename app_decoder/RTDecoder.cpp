#include "RTDecoder.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>
#include <stdexcept>
#include <vector>

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_common/BufferPictureDecMeta.h"
#include "lib_common/BufCommon.h"
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
static inline int RoundUp(int value, int align)
{
    return (value + align - 1) / align * align;
}

static bool IsEndOfStream(AL_TBuffer const *pFrame, AL_TInfoDecode const *pInfo)
{
    return !pFrame && !pInfo;
}

static bool IsReleaseFrame(AL_TBuffer const *pFrame, AL_TInfoDecode const *pInfo)
{
    return pFrame && !pInfo;
}

static bool IsMainDisplay(AL_TInfoDecode const &info)
{
    return info.eOutputID == AL_OUTPUT_MAIN || info.eOutputID == AL_OUTPUT_POSTPROC;
}

static void ConfigureOutputPlaneMode(AL_TPicFormat &pic_format)
{
    if (pic_format.ePlaneMode == AL_PLANE_MODE_MAX_ENUM)
    {
        pic_format.ePlaneMode = GetInternalBufPlaneMode(pic_format.eChromaMode);
    }

    if (pic_format.eComponentOrder == AL_COMPONENT_ORDER_MAX_ENUM)
    {
        pic_format.eComponentOrder = AL_COMPONENT_ORDER_YUV;
    }
}

static size_t ReadSizedChunk(std::ifstream &input, std::ifstream &sizes, AL_TBuffer *pInput, uint8_t &flags)
{
    flags = AL_STREAM_BUF_FLAG_UNKNOWN;

    std::string line;
    if (!std::getline(sizes, line))
    {
        return 0;
    }

    if (line.empty())
    {
        return 0;
    }

    int frame_size = std::stoi(line);
    if (frame_size <= 0)
    {
        return 0;
    }

    const size_t cap = AL_Buffer_GetSize(pInput);
    if (static_cast<size_t>(frame_size) > cap)
    {
        throw std::runtime_error("RTDecoder: split frame size exceeds input buffer capacity");
    }

    auto *dst = AL_Buffer_GetData(pInput);
    input.read(reinterpret_cast<char *>(dst), frame_size);

    if (input.gcount() != frame_size)
    {
        throw std::runtime_error("RTDecoder: bitstream shorter than split size entry");
    }

    flags = AL_STREAM_BUF_FLAG_ENDOFSLICE | AL_STREAM_BUF_FLAG_ENDOFFRAME;
    return static_cast<size_t>(frame_size);
}
}

RTDecoder::RTDecoder(const DecoderConfig &cfg, DecodedFrameCallback cb)
    : m_cfg(cfg), m_frame_cb(std::move(cb))
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

        SetDefaultDecOutputSettings(&m_output_settings);

        int invalid = AL_DecSettings_CheckValidity(&m_dec_settings, nullptr);
        invalid += AL_DecOutputSettings_CheckValidity(&m_output_settings, m_dec_settings.eCodec, nullptr);
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

        m_callbacks.displayCB = { &RTDecoder::s_display, this };
        m_callbacks.resolutionFoundCB = { &RTDecoder::s_resolution_found, this };
        m_callbacks.errorCB = { &RTDecoder::s_error, this };

        auto create_err = AL_Decoder_Create(&m_decoder, m_scheduler, m_allocator, &m_dec_settings, &m_callbacks);
        if (AL_IS_ERROR_CODE(create_err) || !m_decoder)
        {
            throw std::runtime_error(std::string("AL_Decoder_Create failed: ") + AL_Codec_ErrorToString(create_err));
        }

        if (!m_input_pool.Init(m_allocator, m_cfg.input_buffer_num, m_cfg.input_buffer_size, nullptr, "rt_decoder_input"))
        {
            throw std::runtime_error("RTDecoder: failed to init input buffer pool");
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
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_allow_pushback = false;
        m_done = true;
    }
    m_done_cv.notify_all();

    cleanup();
}

void RTDecoder::cleanup()
{
    m_input_pool.Decommit();

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
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_done = true;
    }
    m_done_cv.notify_all();
}

void RTDecoder::signal_error(AL_ERR err)
{
    m_last_error = err;
    signal_done();
}

bool RTDecoder::decode_file(const std::string &bitstream_path, const std::string &split_sizes_path)
{
    std::ifstream input(bitstream_path, std::ios::binary);
    if (!input.is_open())
    {
        throw std::runtime_error("RTDecoder: cannot open input bitstream: " + bitstream_path);
    }

    std::ifstream split_sizes;
    if (m_cfg.input_mode == AL_DEC_SPLIT_INPUT)
    {
        if (split_sizes_path.empty())
        {
            throw std::runtime_error("RTDecoder: split input mode requires a sizes file");
        }

        split_sizes.open(split_sizes_path);
        if (!split_sizes.is_open())
        {
            throw std::runtime_error("RTDecoder: cannot open split sizes file: " + split_sizes_path);
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_done = false;
        m_allow_pushback = true;
        m_num_decoded = 0;
        m_num_concealed = 0;
        m_last_error = AL_SUCCESS;
    }

    bool pool_committed = false;

    try
    {
    m_input_pool.Commit();
    pool_committed = true;

    while (true)
    {
        if (AL_IS_ERROR_CODE(m_last_error.load()))
        {
            throw std::runtime_error("RTDecoder: decoder reported an asynchronous error");
        }

        auto pInput = m_input_pool.GetSharedBuffer(AL_BUF_MODE_BLOCK);
        if (!pInput)
        {
            throw std::runtime_error("RTDecoder: input buffer pool returned null buffer");
        }

        uint8_t flags = AL_STREAM_BUF_FLAG_UNKNOWN;
        size_t read_bytes = 0;

        if (m_cfg.input_mode == AL_DEC_SPLIT_INPUT)
        {
            read_bytes = ReadSizedChunk(input, split_sizes, pInput.get(), flags);
        }
        else
        {
            auto *dst = AL_Buffer_GetData(pInput.get());
            auto cap = AL_Buffer_GetSize(pInput.get());
            input.read(reinterpret_cast<char *>(dst), static_cast<std::streamsize>(cap));
            read_bytes = static_cast<size_t>(input.gcount());
        }

        if (read_bytes == 0)
        {
            break;
        }

        if (!AL_Decoder_PushStreamBuffer(m_decoder, pInput.get(), read_bytes, flags))
        {
            auto err = AL_Decoder_GetLastError(m_decoder);
            m_last_error = err;
            throw std::runtime_error(std::string("RTDecoder: push stream buffer failed: ") + AL_Codec_ErrorToString(err));
        }
    }

    AL_Decoder_Flush(m_decoder);

    std::unique_lock<std::mutex> lock(m_state_mutex);
    bool finished = false;

    if (m_cfg.timeout_ms == 0)
    {
        m_done_cv.wait(lock, [this]() { return m_done; });
        finished = true;
    }
    else
    {
        finished = m_done_cv.wait_for(lock, std::chrono::milliseconds(m_cfg.timeout_ms), [this]() { return m_done; });
    }

    m_input_pool.Decommit();
    pool_committed = false;

    if (!finished)
    {
        m_last_error = AL_ERROR;
        throw std::runtime_error("RTDecoder: timeout while waiting for EOS");
    }

    return !AL_IS_ERROR_CODE(m_last_error.load());
    }
    catch (...)
    {
        if (pool_committed)
        {
            m_input_pool.Decommit();
        }
        throw;
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

AL_ERR RTDecoder::s_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
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

void RTDecoder::s_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo, void *pUserParam)
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

void RTDecoder::s_error(AL_ERR eError, void *pUserParam)
{
    auto *self = static_cast<RTDecoder *>(pUserParam);
    self->on_error(eError);
}

AL_ERR RTDecoder::on_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                      AL_TCropInfo const *pCropInfo)
{
    (void)pCropInfo;

    configure_output_settings(*pStreamSettings);

    if (!AL_Decoder_ConfigureOutputSettings(m_decoder, &m_output_settings))
    {
        return AL_ERR_REQUEST_MALFORMED;
    }

    AL_TDimension out_dim = pStreamSettings->tDim;
    out_dim.iWidth = RoundUp(out_dim.iWidth, 64);
    out_dim.iHeight = RoundUp(out_dim.iHeight, 64);

    const int min_pitch = AL_Decoder_GetMinPitch(out_dim.iWidth, &m_output_settings.tPicFormat);

    if (m_rec_pool_initialized)
    {
        return can_reuse_rec_pool(m_output_settings.tPicFormat, out_dim, min_pitch) ? AL_SUCCESS : AL_ERR_NO_MEMORY;
    }

    configure_rec_pool(m_output_settings.tPicFormat, out_dim, min_pitch);

    const int num_buf = iBufferNumber + 1;
    if (!m_rec_pool.Init(m_allocator, num_buf, "rt_decoder_rec"))
    {
        return AL_ERR_NO_MEMORY;
    }

    for (int i = 0; i < num_buf; ++i)
    {
        auto pDecPict = m_rec_pool.GetSharedBuffer(AL_BUF_MODE_NONBLOCK);
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

    m_rec_pool_initialized = true;
    m_rec_alloc_dim = out_dim;
    m_rec_alloc_pitch_y = min_pitch;
    m_rec_alloc_pic_format = m_output_settings.tPicFormat;
    return AL_SUCCESS;
}

void RTDecoder::on_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo)
{
    if (IsEndOfStream(pFrame, pInfo))
    {
        signal_done();
        return;
    }

    if (IsReleaseFrame(pFrame, pInfo) || !pInfo)
    {
        return;
    }

    AL_Buffer_Ref(pFrame);
    AL_Buffer_InvalidateMemory(pFrame);

    auto frame_err = AL_Decoder_GetFrameError(m_decoder, pFrame);
    if (AL_IS_ERROR_CODE(frame_err))
    {
        signal_error(frame_err);
        AL_Buffer_Unref(pFrame);
        return;
    }
    else if (frame_err == AL_WARN_CONCEAL_DETECT || frame_err == AL_WARN_HW_CONCEAL_DETECT ||
             frame_err == AL_WARN_INVALID_ACCESS_UNIT_STRUCTURE)
    {
        ++m_num_concealed;
    }

    bool is_main = IsMainDisplay(*pInfo);

    if (is_main)
    {
        ++m_num_decoded;

        try
        {
            m_frame_cb(pFrame, *pInfo);
        }
        catch (...)
        {
            signal_error(AL_ERROR);
            AL_Buffer_Unref(pFrame);
            return;
        }
    }

    bool allow_pushback = false;
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        allow_pushback = m_allow_pushback;
    }

    if (is_main && allow_pushback)
    {
        if (!AL_Decoder_PutDisplayPicture(m_decoder, pFrame))
        {
            signal_error(AL_ERROR);
            AL_Buffer_Unref(pFrame);
            return;
        }
    }

    AL_Buffer_Unref(pFrame);
}

void RTDecoder::on_error(AL_ERR eError)
{
    m_last_error = eError;

    if (AL_IS_ERROR_CODE(eError))
    {
        std::lock_guard<std::mutex> lock(m_state_mutex);
        m_allow_pushback = false;
    }

    if (AL_IS_ERROR_CODE(eError))
    {
        signal_done();
    }
}

void RTDecoder::configure_output_settings(AL_TStreamSettings const &stream_settings)
{
    auto &pic_format = m_output_settings.tPicFormat;

    if (pic_format.eChromaMode == AL_CHROMA_MAX_ENUM)
    {
        pic_format.eChromaMode = stream_settings.eChroma;
    }

    pic_format.uBitDepth = static_cast<uint8_t>(stream_settings.iBitDepth);
    pic_format.eStorageMode = AL_FB_RASTER;
    pic_format.bCompressed = false;
    ConfigureOutputPlaneMode(pic_format);

    if (pic_format.eStorageMode == AL_FB_RASTER && pic_format.uBitDepth == 10)
    {
        pic_format.eSamplePackMode = AL_SAMPLE_PACK_MODE_PACKED_XV;
    }
}

bool RTDecoder::can_reuse_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y) const
{
    if (pic_format.eChromaMode != m_rec_alloc_pic_format.eChromaMode ||
        pic_format.uBitDepth != m_rec_alloc_pic_format.uBitDepth ||
        pic_format.eStorageMode != m_rec_alloc_pic_format.eStorageMode ||
        pic_format.bCompressed != m_rec_alloc_pic_format.bCompressed ||
        pic_format.ePlaneMode != m_rec_alloc_pic_format.ePlaneMode ||
        pic_format.eComponentOrder != m_rec_alloc_pic_format.eComponentOrder ||
        pic_format.eSamplePackMode != m_rec_alloc_pic_format.eSamplePackMode)
    {
        return false;
    }

    if (pitch_y > m_rec_alloc_pitch_y)
    {
        return false;
    }

    return dim.iWidth <= m_rec_alloc_dim.iWidth && dim.iHeight <= m_rec_alloc_dim.iHeight;
}

void RTDecoder::configure_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y)
{
    const auto fourcc = AL_GetFourCC(pic_format);
    m_rec_pool.SetFormat(dim, fourcc);

    std::vector<AL_TPlaneDescription> plane_descs;
    AL_EPlaneId used_planes[AL_MAX_BUFFER_PLANES];
    const int num_planes = AL_Plane_GetBufferPixelPlanes(pic_format, used_planes);

    int offset = 0;
    for (int i = 0; i < num_planes; ++i)
    {
        int pitch = (used_planes[i] == AL_PLANE_Y || used_planes[i] == AL_PLANE_YUV)
                        ? pitch_y
                        : AL_GetChromaPitch(fourcc, pitch_y);

        plane_descs.push_back(AL_TPlaneDescription { used_planes[i], offset, pitch });
        offset += AL_DecGetAllocSize_Frame_PixPlane(&pic_format, dim, pitch, used_planes[i]);
    }

    m_rec_pool.AddChunk(static_cast<size_t>(offset), plane_descs);
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
