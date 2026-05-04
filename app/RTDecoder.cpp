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

using AL_BufferGuard = std::unique_ptr<AL_TBuffer, decltype(&AL_Buffer_Unref)>;

RTDecoder::RTDecoder(const DecoderConfig &cfg, DecodedFrameCallback cb)
    : m_cfg(cfg), m_callback(std::move(cb)), m_pAllocator(nullptr), m_pScheduler(nullptr), m_hDec(nullptr),
      m_cbbundles{}, m_lib_initialized(false)
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

        AL_TDecSettings settings{};
        AL_DecSettings_SetDefaults(&settings);
        settings.eCodec = m_cfg.codec;
        settings.eInputMode = m_cfg.input_mode;
        auto result = AL_DecSettings_CheckValidity(&settings, nullptr);
        if (result > 0)
        {
            throw std::runtime_error("AL_DecSettings_CheckValidity found " + std::to_string(result) +
                                     " invalid parameter(s)");
        }

        AL_TDecOutputSettings output_settings{};
        SetDefaultDecOutputSettings(&output_settings);
        result = AL_DecOutputSettings_CheckValidity(&output_settings, settings.eCodec, nullptr);
        if (result != 0)
        {
            throw std::runtime_error("AL_DecOutputSettings_CheckValidity found " + std::to_string(result) +
                                     " invalid parameter(s)");
        }
        m_dec_out_pic_params.tPicFormat = output_settings.tPicFormat;

        result = AL_DecSettings_CheckCoherency(&settings, nullptr);
        if (result < 0)
        {
            throw std::runtime_error("AL_DecSettings_CheckCoherency: fatal incoherency");
        }

        m_cbbundles.displayCB = {&RTDecoder::sdk_display, this};
        m_cbbundles.resolutionFoundCB = {&RTDecoder::sdk_resolution_found, this};
        m_cbbundles.errorCB = {&RTDecoder::sdk_error, this};

        err = AL_Decoder_Create(&m_hDec, m_pScheduler, m_pAllocator, &settings, &m_cbbundles);
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

        throw;
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
        // self->signal_error(AL_ERROR);
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
        self->on_sdk_error(AL_ERROR);
    }
}

void RTDecoder::sdk_error(AL_ERR eError, void *pUserParam)
{
    auto *self = static_cast<RTDecoder *>(pUserParam);
    self->on_sdk_error(eError);
}

AL_ERR RTDecoder::on_sdk_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                          AL_TCropInfo const *pCropInfo)
{
    (void)pCropInfo;
    configure_output_settings(*pStreamSettings);
    AL_TDecOutputSettings output_settings{m_dec_out_pic_params.tPicFormat};
    if (!AL_Decoder_ConfigureOutputSettings(m_hDec, &output_settings))
    {
        return AL_ERR_REQUEST_MALFORMED;
    }

    // Calculate buffer dimensions (rounded up to 64-pixel alignment for hardware)
    AL_TDimension out_dim = pStreamSettings->tDim;
    out_dim.iWidth = RoundUp(out_dim.iWidth, 64);
    out_dim.iHeight = RoundUp(out_dim.iHeight, 64);
    const int min_pitch = AL_Decoder_GetMinPitch(out_dim.iWidth, &m_dec_out_pic_params.tPicFormat);
    if (m_rec_buf_pool)
    {
        return can_reuse_rec_pool({}) ? AL_SUCCESS : AL_ERR_NO_MEMORY;
    }

    m_rec_buf_pool = std::make_unique<PixMapBufPool>();
    configure_rec_pool(m_output_settings.tPicFormat, out_dim, min_pitch);
    const int num_buf = iBufferNumber + 1;
    if (!m_rec_buf_pool->init(m_pAllocator, num_buf, "rt_decoder_rec"))
    {
        return AL_ERR_NO_MEMORY;
    }

    for (int i = 0; i < num_buf; ++i)
    {
        auto pDecPict = m_rec_buf_pool->get_buffer(false);
        if (!pDecPict)
        {
            return AL_ERR_NO_MEMORY;
        }

        AL_Buffer_Cleanup(pDecPict);

        if (!attach_display_metadata(pDecPict))
        {
            AL_Buffer_Unref(pDecPict);
            return AL_ERR_NO_MEMORY;
        }

        if (!AL_Decoder_PutDisplayPicture(m_hDec, pDecPict))
        {
            AL_Buffer_Unref(pDecPict);
            return AL_ERR_REQUEST_MALFORMED;
        }
    }

    // record format for future buffer pool reuse checks
    // out_dim;
    // min_pitch;
    // m_output_settings.tPicFormat;
    return AL_SUCCESS;
}

void RTDecoder::on_sdk_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo)
{
    // end of stream signal (no more frames to decode)
    if (!pFrame && !pInfo)
    {
        // signal_done();
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
        // signal_error(fr_err);
        return;
    }
    else if (fr_err == AL_WARN_CONCEAL_DETECT || fr_err == AL_WARN_HW_CONCEAL_DETECT ||
             fr_err == AL_WARN_INVALID_ACCESS_UNIT_STRUCTURE)
    {
        // m_num_concealed++;
        return;
    }

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
        // signal_error(AL_ERROR);
        return;
    }

    // todo: check reuse conditions before returning buffer to decoder pool
    if (!AL_Decoder_PutDisplayPicture(m_hDec, pFrame))
    {
        // signal_error(AL_ERROR);
        return;
    }
}

void RTDecoder::on_sdk_error(AL_ERR eError)
{
    // signal_error(eError);
}

void RTDecoder::configure_output_settings(AL_TStreamSettings const &stream_settings)
{
    auto &pic_format = m_dec_out_pic_params.tPicFormat;

    if (pic_format.eChromaMode == AL_CHROMA_MAX_ENUM)
    {
        pic_format.eChromaMode = stream_settings.eChroma;
    }

    pic_format.uBitDepth = static_cast<uint8_t>(stream_settings.iBitDepth);
    pic_format.eStorageMode = AL_FB_RASTER;
    pic_format.bCompressed = false;
    if (pic_format.ePlaneMode == AL_PLANE_MODE_MAX_ENUM)
    {
        pic_format.ePlaneMode = GetInternalBufPlaneMode(pic_format.eChromaMode);
    }

    if (pic_format.eComponentOrder == AL_COMPONENT_ORDER_MAX_ENUM)
    {
        pic_format.eComponentOrder = AL_COMPONENT_ORDER_YUV;
    }

    if (pic_format.eStorageMode == AL_FB_RASTER && pic_format.uBitDepth == 10)
    {
        pic_format.eSamplePackMode = AL_SAMPLE_PACK_MODE_PACKED_XV;
    }
}

void RTDecoder::configure_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y)
{
    const auto fourcc = AL_GetFourCC(pic_format);
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

    m_rec_buf_pool->add_chunk(offset, plane_descs);
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

    auto *pDisplayInfoMeta = new AL_TDisplayInfoMetaData();
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

bool RTDecoder::can_reuse_rec_pool(DecOutPicParams const &dec_out_params) const
{
    const auto &pic_format = dec_out_params.tPicFormat;
    const auto &dim = dec_out_params.tDim;
    const int pitch_y = dec_out_params.iPitchY;

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