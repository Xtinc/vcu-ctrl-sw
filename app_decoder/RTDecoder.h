#ifndef REALTIME_DECODER_H
#define REALTIME_DECODER_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

extern "C"
{
#include "lib_decode/lib_decode.h"
#include "lib_decode/DecSettings.h"
}

#include "lib_app/BufPool.h"
#include "lib_app/PixMapBufPool.h"

struct DecoderConfig
{
    AL_ECodec codec = AL_CODEC_HEVC;
    AL_EDecInputMode input_mode = AL_DEC_UNSPLIT_INPUT;
    uint32_t input_buffer_num = 4;
    uint32_t input_buffer_size = 512 * 1024;
    uint32_t timeout_ms = 10000;
    std::string dec_dev_path = "/dev/allegroDecodeIP";
};

class RTDecoder
{
  public:
    using DecodedFrameCallback = std::function<void(AL_TBuffer *pFrame, AL_TInfoDecode const &info)>;

    explicit RTDecoder(const DecoderConfig &cfg, DecodedFrameCallback cb);
    ~RTDecoder();

    RTDecoder(const RTDecoder &) = delete;
    RTDecoder &operator=(const RTDecoder &) = delete;
    RTDecoder(RTDecoder &&) = delete;
    RTDecoder &operator=(RTDecoder &&) = delete;

    bool decode_file(const std::string &bitstream_path, const std::string &split_sizes_path = "");

    int decoded_frames() const;
    int concealed_frames() const;
    AL_ERR last_error() const;

  private:
    void cleanup();
    void signal_done();
    void signal_error(AL_ERR err);
    bool can_reuse_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y) const;

    static AL_ERR s_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                     AL_TCropInfo const *pCropInfo, void *pUserParam);
    static void s_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo, void *pUserParam);
    static void s_error(AL_ERR eError, void *pUserParam);

    AL_ERR on_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                               AL_TCropInfo const *pCropInfo);
    void on_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo);
    void on_error(AL_ERR eError);

    void configure_output_settings(AL_TStreamSettings const &stream_settings);
    void configure_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y);
    bool attach_display_metadata(AL_TBuffer *pDecPict);

  private:
    DecoderConfig m_cfg;
    DecodedFrameCallback m_frame_cb;

    AL_TAllocator *m_allocator = nullptr;
    AL_IDecScheduler *m_scheduler = nullptr;
    AL_HDecoder m_decoder = nullptr;

    AL_TDecSettings m_dec_settings {};
    AL_TDecOutputSettings m_output_settings {};
    AL_TDecCallBacks m_callbacks {};

    BufPool m_input_pool;
    PixMapBufPool m_rec_pool;
    bool m_rec_pool_initialized = false;
    AL_TDimension m_rec_alloc_dim { 0, 0 };
    int m_rec_alloc_pitch_y = 0;
    AL_TPicFormat m_rec_alloc_pic_format {};

    std::mutex m_state_mutex;
    std::condition_variable m_done_cv;
    bool m_done = false;
    bool m_allow_pushback = true;

    std::atomic<int> m_num_decoded { 0 };
    std::atomic<int> m_num_concealed { 0 };
    std::atomic<AL_ERR> m_last_error { AL_SUCCESS };

    bool m_lib_initialized = false;
};

#endif // REALTIME_DECODER_H
