#ifndef REALTIME_DECODER_H
#define REALTIME_DECODER_H

extern "C"
{
#include "lib_decode/DecSettings.h"
#include "lib_decode/lib_decode.h"
}

#include "MemMgr.h"

struct DecoderConfig
{
    AL_ECodec codec = AL_CODEC_HEVC;
    AL_EDecInputMode input_mode = AL_DEC_UNSPLIT_INPUT;
    uint32_t input_buffer_size = 512 * 1024;
    uint32_t timeout_ms = 10000;
    uint32_t input_buffer_num = 4;
    std::string dec_dev_path = "/dev/allegroDecodeIP";
    bool low_delay_mode = false;
};

class RTDecoder
{
    struct DecOutPicParams
    {
        AL_TDimension tDim;
        AL_TPicFormat tPicFormat;
        int iPitchY;
    };

  public:
    using DecodedFrameCallback = std::function<void(AL_TBuffer *pFrame, AL_TInfoDecode const &info)>;

    explicit RTDecoder(const DecoderConfig &cfg, DecodedFrameCallback cb);
    ~RTDecoder();

    RTDecoder(const RTDecoder &) = delete;
    RTDecoder &operator=(const RTDecoder &) = delete;
    RTDecoder(RTDecoder &&) = delete;
    RTDecoder &operator=(RTDecoder &&) = delete;

  private:
    static AL_ERR sdk_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                       AL_TCropInfo const *pCropInfo, void *pUserParam);
    static void sdk_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo, void *pUserParam);
    static void sdk_error(AL_ERR eError, void *pUserParam);

    AL_ERR on_sdk_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                   AL_TCropInfo const *pCropInfo);
    void on_sdk_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo);
    void on_sdk_error(AL_ERR eError);

    void configure_output_settings(AL_TStreamSettings const &stream_settings);
    void configure_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y);
    bool attach_display_metadata(AL_TBuffer *pDecPict);
    bool can_reuse_rec_pool(DecOutPicParams const &dec_out_params) const;

  private:
    mutable std::mutex m_cfg_mutex;
    DecoderConfig m_cfg;
    DecOutPicParams m_dec_out_pic_params;
    // AL_TDecOutputSettings m_output_settings;
    DecodedFrameCallback m_callback;

    AL_TAllocator *m_pAllocator;
    AL_IDecScheduler *m_pScheduler;
    AL_HDecoder m_hDec;

    AL_TDecCallBacks m_cbbundles;

    std::unique_ptr<GenericBufPool> m_src_buf_pool;
    std::unique_ptr<PixMapBufPool> m_rec_buf_pool;

    bool m_lib_initialized;
};

#endif // REALTIME_DECODER_H