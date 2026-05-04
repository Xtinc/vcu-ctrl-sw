#ifndef REALTIME_DECODER_H
#define REALTIME_DECODER_H

// RTDecoder - Real-time H.264/H.265 hardware decoder wrapper
//
// This class wraps the Xilinx/Allegro decoder SDK to provide a push-based
// streaming interface with callback-based frame delivery.
//
// Usage flow:
//   1. Create RTDecoder with config and frame callback.
//   2. Call push_stream() repeatedly to feed bitstream data.
//   3. Frame callback delivers decoded frames asynchronously.
//   4. Call flush() to signal EOS and wait for all frames to drain.
//
// For file-based decoding, use the decode_file() convenience function.

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

extern "C"
{
#include "lib_decode/DecSettings.h"
#include "lib_decode/lib_decode.h"
}

#include "lib_app/BufPool.h"
#include "lib_app/PixMapBufPool.h"

struct DecoderConfig
{
    AL_ECodec codec = AL_CODEC_HEVC;
    AL_EDecInputMode input_mode = AL_DEC_UNSPLIT_INPUT;
    uint32_t input_buffer_num = 4;
    uint32_t input_buffer_size = 512 * 1024;
    uint32_t flush_timeout_ms = 10000;
    std::string dec_dev_path = "/dev/allegroDecodeIP";
};

class RTDecoder
{
  public:
    // Decoded frame callback.
    // Only AL_OUTPUT_MAIN / AL_OUTPUT_POSTPROC frames are delivered.
    // pFrame is valid only for the duration of the callback.
    using DecodedFrameCallback = std::function<void(AL_TBuffer *pFrame, AL_TInfoDecode const &info)>;

    // Throws std::runtime_error on hardware initialisation failure.
    explicit RTDecoder(const DecoderConfig &cfg, DecodedFrameCallback cb);
    ~RTDecoder();

    RTDecoder(const RTDecoder &) = delete;
    RTDecoder &operator=(const RTDecoder &) = delete;
    RTDecoder(RTDecoder &&) = delete;
    RTDecoder &operator=(RTDecoder &&) = delete;

    // Feed a chunk of bitstream data to the decoder.
    // Blocks until an input buffer is available.
    // @param flags  AL_STREAM_BUF_FLAG_* — use AL_STREAM_BUF_FLAG_UNKNOWN for
    //               unsplit mode; ENDOFSLICE|ENDOFFRAME for split mode.
    // Returns false if the decoder is stopped or in an error state.
    bool push_stream(const void *data, size_t size, uint8_t flags = AL_STREAM_BUF_FLAG_UNKNOWN);

    // Signal end of stream and wait for all pending frames to drain.
    // Throws std::runtime_error on timeout. Idempotent.
    void flush();

    const DecoderConfig &config() const
    {
        return m_cfg;
    }
    int decoded_frames() const;
    int concealed_frames() const;
    AL_ERR last_error() const;

  private:
    void cleanup();
    void signal_done();
    void signal_error(AL_ERR err);
    bool can_reuse_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y) const;

    // SDK callback trampolines
    static AL_ERR sdk_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                       AL_TCropInfo const *pCropInfo, void *pUserParam);
    static void sdk_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo, void *pUserParam);
    static void sdk_error(AL_ERR eError, void *pUserParam);

    AL_ERR on_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                               AL_TCropInfo const *pCropInfo);
    void on_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo);
    void on_error(AL_ERR eError);

    // Derives a fully-resolved AL_TDecOutputSettings from stream properties.
    static AL_TDecOutputSettings derive_output_settings(AL_TStreamSettings const &stream_settings);
    void configure_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y);
    bool attach_display_metadata(AL_TBuffer *pDecPict);

    DecoderConfig m_cfg;
    DecodedFrameCallback m_frame_cb;

    AL_TAllocator *m_allocator = nullptr;
    AL_IDecScheduler *m_scheduler = nullptr;
    AL_HDecoder m_decoder = nullptr;

    AL_TDecSettings m_dec_settings{};
    AL_TDecCallBacks m_cbbundles{};

    std::unique_ptr<BufPool> m_input_pool;
    std::unique_ptr<PixMapBufPool> m_rec_pool;

    // Allocation parameters used when m_rec_pool was created; valid only when
    // m_rec_pool != nullptr. Used to decide whether existing buffers can be reused
    // on subsequent resolution-found events.
    struct RecPoolAlloc
    {
        AL_TPicFormat pic_format{};
        AL_TDimension dim{0, 0};
        int pitch_y = 0;
    } m_rec_pool_alloc;

    std::atomic<bool> m_stopped{false};
    std::atomic<bool> m_allow_pushback{true};

    std::mutex m_eos_mutex;
    std::condition_variable m_eos_cv;
    bool m_eos_signaled = false;

    std::atomic<int> m_num_decoded{0};
    std::atomic<int> m_num_concealed{0};
    std::atomic<AL_ERR> m_last_error{AL_SUCCESS};

    bool m_lib_initialized = false;
};

// Convenience: decode a complete H.264/H.265 bitstream file.
// split_sizes_path is required when cfg.input_mode == AL_DEC_SPLIT_INPUT.
// Throws std::runtime_error on I/O errors or decode timeout.
void decode_file(RTDecoder &decoder, const std::string &bitstream_path, const std::string &split_sizes_path = "");

#endif // REALTIME_DECODER_H
