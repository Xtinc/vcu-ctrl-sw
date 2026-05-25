#ifndef DEC_MGR_H
#define DEC_MGR_H

#include "DRMDisplay.h"
#include "RTDecoder.h"

/**
 * @brief DecMgrConfig holds all configuration needed to create a DecMgr instance.
 */
struct DecMgrConfig
{
    DecoderConfig dec;    ///< Hardware decoder parameters.
    DRMDisplayConfig drm; ///< DRM/KMS display configuration.
};

/**
 * @brief DecMgr is the top-level decode+display manager.
 *
 * Owns both the hardware decoder (m_decoder) and the DRM/KMS display (m_display),
 * and coordinates their lifecycle via an explicit state machine:
 *
 *   Running:
 *     push_stream() ok          -> stays Running
 *     push_stream() fails       -> DecoderFault
 *     flush() / stop()          -> Stopping
 *   DecoderFault:
 *     rebuild() ok              -> Running
 *     rebuild() fail            -> Stopping
 *     stop()                    -> Stopping
 *   Stopping:
 *     terminal state; flush decoder EOS, stop display, destroy both
 *
 * ### Display lifecycle
 * The DRMDisplay is created **lazily** on the first decoded frame so that the
 * correct format and dimensions are known at open time.  After a rebuild() the
 * display is torn down together with the old decoder and will be re-created on
 * the first frame from the new decoder session.
 *
 * ### Buffer ownership
 * - Decoded AL_TBuffer frames are owned by the decoder's rec pool.
 * - on_decoded_frame() shows each frame on the display.
 * - The display's release callback returns the frame to its originating decoder
 *   via return_display_frame().
 * - A two-phase decoder construction (cb_ctx pattern) gives each codec session
 *   its own weak_ptr identity so that frames are always returned to the correct
 *   decoder even when a rebuild is in progress.
 *
 * ### Drain order
 * Always:  decoder.flush()  →  display.stop()  →  decoder.reset()
 * This mirrors the pattern established in main_dec.cpp and ensures the
 * display can return its last held frame to the decoder before the decoder
 * pool is freed.
 *
 * ### Thread safety
 * - push_stream() may be called from any single producer thread.
 * - The SDK delivers decoded frames from an internal thread; that thread calls
 *   on_decoded_frame() which may lazily create the display.
 * - stop() / flush() / rebuild() must not be called concurrently with each other.
 * - set_bitrate / set_framerate are not applicable to decoding; for symmetry
 *   with EncMgr, fps() returns the decoder EMA throughput.
 */
class DecMgr
{
    enum class State
    {
        Running,      ///< Decoder active, display showing frames.
        DecoderFault, ///< Decoder stopped unexpectedly; call rebuild() before resuming.
        Stopping,     ///< Terminal: flush decoder, stop display, exit.
    };

  public:
    /**
     * @brief Construct a DecMgr. Does not start decoding; call start().
     * @param cfg  Configuration for decoder and display.
     */
    explicit DecMgr(DecMgrConfig cfg);

    /** @brief Destructor. Calls stop() if still running. */
    ~DecMgr();

    DecMgr(const DecMgr &) = delete;
    DecMgr &operator=(const DecMgr &) = delete;
    DecMgr(DecMgr &&) = delete;
    DecMgr &operator=(DecMgr &&) = delete;

    /**
     * @brief Initialise the decoder pipeline (non-blocking).
     * @return true on success, false if already running or decoder creation fails.
     */
    bool start();

    /**
     * @brief Gracefully stop the pipeline (blocking).
     *
     * Flushes the decoder to EOS, drains the display event thread, then
     * destroys both objects in the correct order.
     * Safe to call multiple times or after a previous stop().
     */
    void stop();

    /**
     * @brief Push a chunk of compressed bitstream data.
     *
     * Thread-safe with respect to the SDK callback thread.
     * Returns false when the decoder has stopped or an internal error occurred;
     * the caller should then call rebuild() to recover, or stop() to exit.
     *
     * @param data   Pointer to bitstream bytes. Must not be null.
     * @param size   Number of bytes. Must not exceed DecoderConfig::input_buffer_size.
     * @param flags  Stream buffer flags (default: AL_STREAM_BUF_FLAG_UNKNOWN).
     * @return true on success; false on decoder fault or pipeline stopped.
     */
    bool push_stream(const void *data, size_t size, uint8_t flags = AL_STREAM_BUF_FLAG_UNKNOWN);

    /**
     * @brief Signal end-of-stream, wait for all frames, then stop the pipeline.
     *
     * Sends an EOS marker to the hardware decoder, blocks until every pending
     * decoded frame has been delivered to the display and presented, then
     * performs a full stop() (identical drain order).
     *
     * @return true on clean EOS; false on timeout (output may be incomplete).
     */
    bool flush();

    /**
     * @brief Rebuild the decoder pipeline after a DecoderFault.
     *
     * Tears down the current decoder and display, then re-creates the decoder.
     * The display will be re-opened lazily on the first frame from the new session.
     *
     * @return true on success; false if the new decoder cannot be created
     *         (the pipeline transitions to Stopping in that case).
     */
    bool rebuild();

    /**
     * @brief Return the latest EMA decode frame rate.
     * @return fps (0.0 until 100 frames decoded or while stopped).
     */
    double fps() const;

  private:
    /// Weak reference to a specific RTDecoder instance (used for per-session lifetime tracking).
    using DecRef = std::weak_ptr<RTDecoder>;

    /**
     * @brief SDK frame delivery callback, dispatched from the SDK internal thread.
     *
     * @param frame  Decoded picture buffer.
     * @param info   Display metadata.
     * @param orig   Weak reference to the decoder that produced this frame; used to
     *               return the frame when no display is available.
     */
    void on_decoded_frame(AL_TBuffer *frame, const AL_TInfoDecode &info, const DecRef &orig);

    /**
     * @brief Allocate and wire a new RTDecoder instance into m_decoder.
     *
     * Uses the cb_ctx two-phase pattern so that the callback closure captures a
     * weak_ptr to its own decoder (self-reference without a reference cycle).
     *
     * @return true on success.
     */
    bool create_decoder();

  private:
    DecMgrConfig m_cfg;

    mutable std::mutex m_dec_mutex;
    std::shared_ptr<RTDecoder> m_decoder; ///< shared_ptr so release callbacks can hold a weak copy.

    std::mutex m_disp_mutex;
    std::unique_ptr<DRMDisplay> m_display; ///< Created lazily on the first decoded frame.

    std::atomic<bool> m_running{false};
    std::atomic<State> m_state{State::Stopping};
};

#endif // DEC_MGR_H
