#ifndef DEC_MGR_H
#define DEC_MGR_H

#include "DRMDisplay.h"
#include "RTDecoder.h"

/**
 * @brief Configuration bundle for DecMgr initialization.
 *
 * Holds all parameters needed to create and configure the decode+display pipeline.
 * Pass this struct to DecMgr's constructor.
 *
 * @par Example
 * @code
 *   DecMgrConfig cfg;
 *   cfg.dec.codec = AL_CODEC_HEVC;
 *   cfg.dec.dec_dev_path = "/dev/allegroDecodeIP";
 *   cfg.dec.input_buffer_size = 1024 * 1024;  // 1 MB
 *   cfg.drm.drm_device = "/dev/dri/card0";
 *   cfg.drm.desired_width = 1920;
 *   cfg.drm.desired_height = 1080;
 *
 *   DecMgr mgr(cfg);
 * @endcode
 */
struct DecMgrConfig
{
    DecoderConfig dec;    ///< Hardware decoder configuration (codec, device, buffer sizes).
    DRMDisplayConfig drm; ///< DRM/KMS display configuration (device, mode, timing).
};

/**
 * @brief Top-level decode+display pipeline with automatic error recovery.
 *
 * DecMgr orchestrates hardware video decoding and DRM/KMS display output in a
 * zero-copy pipeline.  Compressed bitstream pushed via push_stream() is decoded
 * by the VCU and rendered to the display without intermediate CPU copies.
 *
 * @par Typical Usage
 * @code
 *   DecMgrConfig cfg;
 *   cfg.dec.codec = AL_CODEC_HEVC;
 *   cfg.drm.drm_device = "/dev/dri/card0";
 *
 *   DecMgr mgr(cfg);
 *   if (!mgr.start()) {
 *       // handle error
 *   }
 *
 *   while (streaming) {
 *       mgr.push_stream(chunk.data(), chunk.size());
 *   }
 *
 *   mgr.stop();  // or let destructor call it
 * @endcode
 *
 * @par Lifecycle & Resource Management
 * The pipeline follows a strict four-phase lifecycle:
 *
 * 1. **Construction**: Resources not allocated; call start().
 * 2. **Running**: Decoder and display active; push_stream() accepts data.
 * 3. **Draining**: Decoder flushed; pending frames being displayed.
 * 4. **Stopped**: All resources released; safe to destroy.
 *
 * Destruction order (in stop() or destructor):
 * @code
 *   decoder.flush()    →  // Wait for all decoded frames
 *   display.drain()    →  // Wait for all display callbacks to complete
 *   display.reset()    →  // Destroy display
 *   decoder.reset()    →  // Destroy decoder
 * @endcode
 *
 * @par Lifetime Guarantees
 * - **Decoder always outlives Display**: Display is destroyed before Decoder,
 *   ensuring display callbacks (return_frame) never access a destroyed decoder.
 * - **drain() is a synchronization barrier**: After drain() returns, no more
 *   display callbacks will execute, making subsequent destruction safe.
 * - **No shared_ptr overhead**: Explicit lifetime management eliminates the need
 *   for reference counting or runtime null checks.
 *
 * @par Error Recovery
 * Decoder errors (e.g., corrupted bitstream, resource exhaustion) are handled
 * transparently:
 * - push_stream() detects decoder failure
 * - Automatically rebuilds decoder in-place (display kept alive)
 * - Retries the push operation
 * - Caller sees only a boolean success/failure
 *
 * @par Buffer Ownership
 * - Decoded frames (AL_TBuffer) are owned by the decoder's reconstruction pool.
 * - Display imports DMA-buf handles via prepare_fb (zero-copy).
 * - Each frame is reference-counted; returned to pool via return_display_frame().
 * - Display callbacks (return_frame) invoke decoder's return path when page-flips complete.
 *
 * @par Resolution Changes
 * Dynamic resolution changes are handled transparently:
 * - prepare_fb re-imports DMA-buf with new dimensions
 * - Atomic flip commits carry updated CRTC_W/H
 * - No modeset or display restart required
 *
 * @par Thread Safety
 * - push_stream() must be called from a single producer thread.
 * - stop() must not overlap with push_stream().
 * - fps() may be called from any thread at any time.
 * - Internal callbacks execute on SDK thread (on_decoded_frame) and DRM event thread (return_frame).
 *
 * @note The class is non-copyable and non-movable.
 * @warning Calling push_stream() concurrently from multiple threads is undefined behavior.
 */
class DecMgr
{
  public:
    /**
     * @brief Construct a DecMgr instance.
     *
     * Resources are not allocated during construction. Call start() to
     * initialize the decoder and display pipeline.
     *
     * @param cfg  Configuration for hardware decoder and DRM/KMS display.
     *             The config is copied internally.
     *
     * @note Constructor never throws; all initialization happens in start().
     */
    explicit DecMgr(DecMgrConfig cfg);

    /**
     * @brief Destructor.
     *
     * Automatically calls stop() if the pipeline is still running,
     * ensuring graceful shutdown and resource cleanup.
     *
     * @note Blocks until all frames are displayed and resources released.
     */
    ~DecMgr();

    DecMgr(const DecMgr &) = delete;
    DecMgr &operator=(const DecMgr &) = delete;
    DecMgr(DecMgr &&) = delete;
    DecMgr &operator=(DecMgr &&) = delete;

    /**
     * @brief Initialize the decoder+display pipeline.
     *
     * Allocates and initializes the DRM/KMS display and hardware decoder.
     * Opens the VCU device, creates buffer pools, and starts the DRM event thread.
     *
     * @return true   Pipeline successfully started and ready to accept data.
     * @return false  Initialization failed, or start() was already called.
     *
     * @note Call this exactly once after construction, before push_stream().
     * @note This function does not block; decoding starts when push_stream() is called.
     *
     * @par Failure reasons
     * - Pipeline already running (start() called twice)
     * - DRM device cannot be opened (permission/availability)
     * - VCU decoder device unavailable or in use
     * - Buffer allocation failure (out of DMA memory)
     */
    bool start();

    /**
     * @brief Gracefully stop the pipeline and release all resources.
     *
     * Performs a four-step shutdown sequence:
     * 1. Flush decoder: wait for all pending frames to be decoded.
     * 2. Drain display: wait for all page-flips to complete.
     * 3. Destroy display: release DRM/KMS resources.
     * 4. Destroy decoder: release VCU hardware and buffer pools.
     *
     * @note This function blocks until all frames have been displayed.
     * @note Safe to call multiple times (subsequent calls are no-ops).
     * @note Automatically called by the destructor if still running.
     * @note After stop(), the pipeline can be restarted by calling start() again.
     *
     * @warning Do not call concurrently with push_stream().
     *
     * @par Timeout behavior
     * If decoder flush times out (default 5s), stop() logs a warning but
     * continues with display drain to avoid resource leaks. Incomplete
     * frames may be dropped.
     */
    void stop();

    /**
     * @brief Submit compressed bitstream data for decoding.
     *
     * Copies the provided data into a DMA buffer and submits it to the
     * hardware decoder. Decoded frames are automatically forwarded to the
     * display. If the decoder encounters an error, it is automatically
     * rebuilt and the operation retried transparently.
     *
     * @param data   Pointer to compressed bitstream bytes. Must not be nullptr.
     * @param size   Number of bytes to push. Must be > 0 and ≤ input_buffer_size.
     * @param flags  Stream buffer flags (e.g., AL_STREAM_BUF_FLAG_ENDOFSLICE).
     *               Default: AL_STREAM_BUF_FLAG_UNKNOWN.
     *
     * @return true   Data successfully submitted to decoder.
     * @return false  Pipeline stopped, decoder rebuild failed, or invalid parameters.
     *
     * @note This function may block briefly if all input buffers are in use.
     * @note Must be called from a single producer thread; not thread-safe.
     * @note On decoder error, rebuilds decoder and retries once before returning false.
     *
     * @warning data must remain valid for the duration of this call (synchronous copy).
     * @warning size must not exceed DecoderConfig::input_buffer_size (default 512 KB).
     *
     * @par Error recovery
     * If the decoder fails (corrupted stream, resource exhaustion):
     * 1. Current decoder is flushed and destroyed
     * 2. Display is drained (pending frames displayed)
     * 3. Fresh decoder is created with same configuration
     * 4. Push operation is retried automatically
     * 5. Returns false only if rebuild fails or pipeline is stopped
     */
    bool push_stream(const void *data, size_t size, uint8_t flags = AL_STREAM_BUF_FLAG_UNKNOWN);

    /**
     * @brief Get the current decode frame rate.
     *
     * Returns an exponential moving average (EMA, α=0.9) of the decoder's
     * output frame rate. The rate is computed every 100 frames.
     *
     * @return Decode frame rate in frames per second (fps).
     * @retval 0.0  Pipeline stopped, or fewer than 100 frames decoded.
     *
     * @note Thread-safe; may be called from any thread at any time.
     * @note Only counts main/postproc output frames, not auxiliary outputs.
     */
    double fps() const;

  private:
    void on_decoded_frame(AL_TBuffer *frame, const AL_TInfoDecode &info);
    void return_frame(AL_TBuffer *frame);
    bool create_decoder();
    bool do_rebuild();

  private:
    DecMgrConfig m_cfg;
    mutable std::mutex m_dec_mutex;
    std::unique_ptr<RTDecoder> m_decoder;
    std::unique_ptr<DRMDisplay> m_display;
    std::atomic<bool> m_running{false};
};

#endif // DEC_MGR_H
