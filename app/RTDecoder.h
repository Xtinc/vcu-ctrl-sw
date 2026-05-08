#ifndef REALTIME_DECODER_H
#define REALTIME_DECODER_H

extern "C"
{
#include "lib_decode/DecSettings.h"
#include "lib_decode/lib_decode.h"
}

#include "MemMgr.h"
#include <atomic>
#include <chrono>
#include <unordered_map>

class LatencyMeasurer;

/**
 * @brief Configuration parameters for RTDecoder.
 *
 * All fields carry sensible defaults for single-stream ZYNQ VCU HEVC decoding.
 * Construct one of these, adjust the fields you need, then pass it to the
 * RTDecoder constructor.
 */
struct DecoderConfig
{
    AL_ECodec codec = AL_CODEC_HEVC;                   ///< Video codec (HEVC or AVC).
    uint32_t input_buffer_size = 512 * 1024;           ///< Size in bytes of each input stream buffer.
    uint32_t input_buffer_num = 4;                     ///< Number of input stream buffers in the pool.
    std::string dec_dev_path = "/dev/allegroDecodeIP"; ///< Device file path of the VCU decode IP.
    bool low_delay_mode = false;                       ///< Enable low-latency profile (split input + VCL unit decode).
    uint32_t flush_timeout_ms = 5000;                  ///< Timeout in milliseconds while waiting for decoder EOS.
};

/**
 * @brief Real-time hardware-accelerated video decoder for Xilinx ZYNQ VCU.
 *
 * RTDecoder wraps the Allegro DVT decode SDK into a push-based, thread-safe
 * interface.  It owns the full decode pipeline: DMA allocator, MCU scheduler,
 * stream buffer pool, and decoded-picture (rec) buffer pool.
 *
 * Lifecycle state machine (single atomic state):
 *
 *   RUNNING:
 *     push_stream()         -> submits bitstream data, stays in RUNNING
 *     flush()               -> transitions to FLUSHING
 *     fatal SDK error       -> signal_done() -> DONE
 *     destructor            -> flush() then STOPPING
 *   FLUSHING:
 *     EOS callback received -> signal_done() -> DONE
 *     fatal SDK error       -> signal_done() -> DONE
 *     flush() CV wakes      -> caller returns (state == DONE)
 *   DONE:
 *     flush() returns to caller
 *     destructor sets       -> STOPPING
 *   STOPPING:
 *     terminal state set by destructor before AL_Decoder_Destroy;
 *     all rec-buffer return calls (PutDisplayPicture) are suppressed in this state
 *
 * ### Typical usage
 * @code
 *   DecoderConfig cfg;
 *   cfg.codec = AL_CODEC_HEVC;
 *
 *   RTDecoder dec(cfg, [](AL_TBuffer* frame, const AL_TInfoDecode& info) {
 *       // Called from an SDK-internal thread for every decoded frame.
 *       // The frame pointer is valid only for the duration of this callback.
 *       display(frame);
 *   });
 *
 *   while (source.has_data())
 *       dec.push_stream(chunk.data(), chunk.size());
 *
 *   dec.flush(); // blocks until all frames have been decoded and delivered
 * @endcode
 *
 * ### Thread safety
 * - push_stream() may be called from any single producer thread.
 * - flush() must be called from the same thread as push_stream(), or after
 *   all push_stream() calls have returned.
 * - The DecodedFrameCallback is invoked from an SDK-internal thread; the
 *   implementation must be thread-safe with respect to the caller.
 *
 * @note The class is non-copyable and non-movable.
 * @throws std::runtime_error  If any SDK resource cannot be initialised.
 */
class RTDecoder
{
    struct RecPoolAlloc
    {
        AL_TPicFormat pic_format{};
        AL_TDimension dim{0, 0};
        int pitch_y = 0;
    };

  public:
    /**
     * @brief Callback invoked for every successfully decoded output frame.
     *
     * The callback is called from an SDK-internal thread.  @p pFrame is
     * reference-counted: it remains valid for the duration of the call and
     * must not be stored beyond it without an explicit AL_Buffer_Ref().
     *
     * @param pFrame  Decoded picture buffer (PixMap format).
     * @param info    Display metadata: dimensions, crop, output ID, etc.
     */
    using DecodedFrameCallback = std::function<void(AL_TBuffer *pFrame, AL_TInfoDecode const &info)>;

    /**
     * @brief Construct and fully initialise the decoder pipeline.
     *
     * Initialises the Allegro decode library, creates the DMA allocator and
     * MCU scheduler for @p cfg.dec_dev_path, validates decoder settings, and
     * allocates the input stream buffer pool.  The decoded-picture (rec) pool
     * is allocated lazily on the first resolution-found callback.
     *
     * @param cfg  Decoder configuration.  Copied internally.
     * @param cb   Frame delivery callback.  Must not be null.
     * @throws std::invalid_argument  If @p cb is null.
     * @throws std::runtime_error     On any SDK or resource initialisation failure.
     */
    explicit RTDecoder(const DecoderConfig &cfg, DecodedFrameCallback cb);

    /**
     * @brief Flush any pending frames and release all SDK resources.
     *
     * If flush() has not been called explicitly, the destructor calls it
     * internally (ignoring timeout errors) before tearing down the pipeline.
     */
    ~RTDecoder();

    RTDecoder(const RTDecoder &) = delete;
    RTDecoder &operator=(const RTDecoder &) = delete;
    RTDecoder(RTDecoder &&) = delete;
    RTDecoder &operator=(RTDecoder &&) = delete;

    /**
     * @brief Push a chunk of compressed bitstream data into the decoder.
     *
     * Copies @p size bytes from @p data into an internal DMA buffer and
     * submits it to the hardware decoder.  Blocks briefly if all stream
     * buffers are currently in use by the hardware.
     *
     * @param data   Pointer to the bitstream data.  Must not be null.
     * @param size   Number of bytes to submit.  Must not exceed
     *               DecoderConfig::input_buffer_size.
     * @param flags  Stream buffer flags (e.g. AL_STREAM_BUF_FLAG_ENDOFSLICE).
     *               Defaults to AL_STREAM_BUF_FLAG_UNKNOWN.
     * @return @c true on success; @c false if the decoder has been stopped,
     *         an error has occurred, or @p size exceeds the buffer capacity.
     */
    bool push_stream(const void *data, size_t size, uint8_t flags = AL_STREAM_BUF_FLAG_UNKNOWN);

    /**
     * @brief Signal end-of-stream and wait for all frames to be delivered.
     *
     * Sends an EOS marker to the hardware decoder and blocks until every
     * pending decoded frame has been delivered via the DecodedFrameCallback,
     * or until DecoderConfig::flush_timeout_ms elapses.
     *
     * Never throws. Safe to call after a network disconnect or any abnormal condition.
     *
     * @return @c true  EOS received — all frames have been delivered.
     *         @c false EOS wait timed out (5 s).
     * @note Calling flush() more than once is a no-op after the first call
     *       and always returns @c true.
     */
    bool flush();

    /**
     * @brief Return the latest exponential-moving-average decode frame rate.
     *
     * Updated every 100 output frames (main/postproc output only).
     * Returns 0.0 until 100 frames have been decoded.
     *
     * @return Decode fps (EMA, α=0.9). Thread-safe.
     */
    double fps() const;

  private:
    static AL_ERR sdk_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                       AL_TCropInfo const *pCropInfo, void *pUserParam);
    static void sdk_end_decoding(AL_TBuffer *pFrame, void *pUserParam);
    static void sdk_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo, void *pUserParam);
    static void sdk_error(AL_ERR eError, void *pUserParam);
    static void sdk_end_parsing(AL_TBuffer *pParsedFrame, void *pUserParam, int iParsingId);
    static void sdk_parsed_sei(bool is_prefix, int payload_type, uint8_t *payload, int payload_size, void *pUserParam);
    AL_ERR on_sdk_resolution_found(int iBufferNumber, AL_TStreamSettings const *pStreamSettings,
                                   AL_TCropInfo const *pCropInfo);
    void on_sdk_display(AL_TBuffer *pFrame, AL_TInfoDecode *pInfo);
    void on_sdk_error(AL_ERR eError);
    void on_sdk_end_parsing(AL_TBuffer *pParsedFrame, int iParsingId);
    void on_sdk_parsed_sei(bool is_prefix, int payload_type, uint8_t *payload, int payload_size);

    static AL_TDecOutputSettings derive_output_settings(AL_TStreamSettings const &stream_settings);
    void configure_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y);
    bool attach_display_metadata(AL_TBuffer *pDecPict);
    bool can_reuse_rec_pool(AL_TPicFormat const &pic_format, AL_TDimension const &dim, int pitch_y) const;

    void signal_done();                                 // transitions to Done and wakes flush() waiter
    void signal_error(AL_ERR err);                      // logs error then calls signal_done()
    void update_fps();                                  // called from on_sdk_display per output frame
    void cleanup();

  private:
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
    std::unique_ptr<LatencyMeasurer> m_sei_measurer;

    std::atomic<double> m_fps;
    uint32_t m_frame_count;
    std::chrono::steady_clock::time_point m_fps_last_time;

    enum class State : uint8_t
    {
        Running = 0,  ///< Normal operation: accepts input, allows rec-buffer return
        Flushing = 1, ///< Draining: no new input accepted, rec-buffer return still allowed
        Done = 2,     ///< Terminal: EOS received or fatal error; wakes flush() waiter
        Stopping = 3, ///< Destructor guard: set before AL_Decoder_Destroy; PutDisplayPicture suppressed
    };
    std::atomic<State> m_state;

    std::mutex m_eos_mutex;
    std::condition_variable m_eos_cv;
    bool m_lib_initialized;
};

#endif // REALTIME_DECODER_H