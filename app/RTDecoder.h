#ifndef REALTIME_DECODER_H
#define REALTIME_DECODER_H

extern "C"
{
#include "lib_decode/DecSettings.h"
#include "lib_decode/lib_decode.h"
}

#include "MemMgr.h"
#include <atomic>

struct DecoderConfig
{
    AL_ECodec codec = AL_CODEC_HEVC;
    AL_EDecInputMode input_mode = AL_DEC_UNSPLIT_INPUT;
    uint32_t input_buffer_size = 512 * 1024;
    uint32_t flush_timeout_ms = 10000;
    uint32_t input_buffer_num = 4;
    std::string dec_dev_path = "/dev/allegroDecodeIP";
    bool low_delay_mode = false;
};

class RTDecoder
{
    struct RecPoolAlloc
    {
        AL_TPicFormat pic_format{};
        AL_TDimension dim{0, 0};
        int pitch_y = 0;
    };

  public:
    using DecodedFrameCallback = std::function<void(AL_TBuffer *pFrame, AL_TInfoDecode const &info)>;

    explicit RTDecoder(const DecoderConfig &cfg, DecodedFrameCallback cb);
    ~RTDecoder();

    RTDecoder(const RTDecoder &) = delete;
    RTDecoder &operator=(const RTDecoder &) = delete;
    RTDecoder(RTDecoder &&) = delete;
    RTDecoder &operator=(RTDecoder &&) = delete;

    bool push_stream(const void *data, size_t size, uint8_t flags = AL_STREAM_BUF_FLAG_UNKNOWN);

    void flush();

  private:
    static AL_ERR sdk_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                       AL_TCropInfo const *pCropInfo, void *pUserParam);
    static void sdk_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo, void *pUserParam);
    static void sdk_error(AL_ERR eError, void *pUserParam);
    AL_ERR on_sdk_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                   AL_TCropInfo const *pCropInfo);
    void on_sdk_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo);
    void on_sdk_error(AL_ERR eError);

    static AL_TDecOutputSettings derive_output_settings(AL_TStreamSettings const &stream_settings);
    void configure_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y);
    bool attach_display_metadata(AL_TBuffer *pDecPict);
    bool can_reuse_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y) const;

    void signal_done();
    void signal_error(AL_ERR err);
    void cleanup();

  private:
    mutable std::mutex m_cfg_mutex;
    DecoderConfig m_cfg;
    DecodedFrameCallback m_callback;

    AL_TAllocator *m_pAllocator;
    AL_IDecScheduler *m_pScheduler;
    AL_HDecoder m_hDec;

    AL_TDecSettings m_dec_settings;
    RecPoolAlloc m_rec_pool_alloc;
    AL_TDecCallBacks m_cbbundles;

    std::unique_ptr<GenericBufPool> m_src_buf_pool;
    std::unique_ptr<PixMapBufPool> m_rec_buf_pool;

    std::atomic<bool> m_stopped;
    std::atomic<bool> m_allow_pushback;
    std::mutex m_eos_mutex;
    std::condition_variable m_eos_cv;
    bool m_eos_signaled;

    std::atomic<bool> m_error;
    bool m_lib_initialized;
};

#endif // REALTIME_DECODER_H