#ifndef ENC_MGR_H
#define ENC_MGR_H

#include "RTEncoder.h"
#include "V4L2Source.h"

/**
 * @brief EncMgrConfig holds all configuration needed to create an EncMgr instance.
 */
struct EncMgrConfig
{
    EncoderConfig enc;             ///< Encoder parameters (width/height = initial capture resolution)
    std::string v4l2_dev;          ///< V4L2 capture device path, e.g. "/dev/video0"
    std::string sync_dev;          ///< Xilinx sync device path (empty = disabled)
    int reconnect_delay_ms = 2000; ///< Delay between reconnection attempts in milliseconds
    int max_reconnect_tries = -1;  ///< Max consecutive reconnect attempts (-1 = unlimited)
};

/**
 * @brief EncMgr is the top-level encoding manager.
 *
 * Owns both the V4L2 capture source (m_source) and the hardware encoder (m_encoder),
 * and coordinates their lifecycle via an explicit state machine:
 *
 *   Opening:
 *     open_source ok          -> Streaming
 *     open_source fail        -> Reconnecting
 *     stop()                  -> Stopping
 *   Streaming:
 *     dqueue/submit loop active
 *     stop() / normal exit    -> Stopping
 *     V4L2 device error       -> Reconnecting
 *     encoder fault           -> EncoderFault
 *     V4L2_EVENT_SOURCE_CHANGE -> SourceChanged
 *   SourceChanged:
 *     set_resolution() ok     -> Opening  (no delay)
 *     set_resolution() fail   -> rebuild_encoder() -> Opening  (no delay)
 *     rebuild_encoder() fail  -> Stopping
 *     stop()                  -> Stopping
 *   EncoderFault:
 *     rebuild_encoder() ok    -> Opening  (no delay)
 *     rebuild_encoder() fail  -> Stopping
 *     stop()                  -> Stopping
 *   Reconnecting:
 *     wait_reconnect() ok     -> Opening
 *     max retries exceeded    -> Stopping
 *     stop()                  -> Stopping
 *   Stopping:
 *     terminal state; flush encoder EOS, destroy encoder, exit loop thread
 *
 * The encoder is created once in start() and kept alive across reconnect cycles;
 * only m_source is torn down and rebuilt each time. m_encoder is flushed (EOS) once
 * at final stop, not between sessions, so the encode session remains continuous.
 *
 * Threading model:
 *   - start() spawns one loop thread that drives the entire pipeline.
 *   - stop() signals the thread and joins it (blocking until encoder EOS drains).
 *   - set_bitrate / set_framerate / request_IDR / fps are only accepted while the
 *     pipeline is running; they acquire m_enc_mutex, which also synchronises with
 *     the loop thread's final encoder reset.
 *
 * Encoded output is currently wired to a local placeholder callback. A future
 * network sender will be connected here once that class exists.
 */
class EncMgr
{
  public:
    /**
     * @brief Construct an EncMgr. Does not start encoding; call start().
     * @param cfg       Configuration for encoder and capture device.
     * @throw std::invalid_argument if cfg.v4l2_dev is empty.
     */
    explicit EncMgr(EncMgrConfig cfg);

    /** @brief Destructor. Calls stop() if still running. */
    ~EncMgr();

    EncMgr(const EncMgr &) = delete;
    EncMgr &operator=(const EncMgr &) = delete;
    EncMgr(EncMgr &&) = delete;
    EncMgr &operator=(EncMgr &&) = delete;

    /**
     * @brief Start the encode pipeline (non-blocking).
     * @return true on success, false if already running or encoder creation fails.
     */
    bool start();

    /**
     * @brief Stop the encode pipeline (blocking).
     *
     * Signals the loop thread, waits for it to exit (V4L2 dequeue timeout ≤ 1 s,
     * encoder EOS flush ≤ 5 s), then destroys the encoder.
     * Safe to call from any thread or after a previous stop().
     */
    void stop();

    /**
    * @brief Dynamically update the target (and optionally peak) bitrate.
    *
    * Only valid while the pipeline is running.
     * @param target Target bitrate in bps. Must be > 0.
     * @param max    VBR peak bitrate in bps. 0 = same as target (CBR).
     * @return true on success, false if encoder is not available or SDK rejects the value.
     */
    bool set_bitrate(uint32_t target, uint32_t max = 0);

    /**
     * @brief Dynamically update the encode frame rate.
    *
    * Only valid while the pipeline is running.
     * @param fps Frame rate numerator. Must be > 0.
     * @param clk Frame rate denominator. Must be > 0.
     * @return true on success, false if encoder is not available or SDK rejects the value.
     */
    bool set_framerate(uint32_t fps, uint32_t clk = 1000);

      /** @brief Force the encoder to insert an IDR frame at the next opportunity while running. */
    void request_IDR();

    /**
     * @brief Return the latest EMA throughput statistics.
    *
    * Returns {0.0, 0.0} while the pipeline is stopped.
     * @return {fps, bitrate_bps}. Values are 0.0 until at least 100 frames have been encoded.
     */
    std::pair<double, double> fps() const;

  private:
    // ---------------------------------------------------------------------------
    // Pipeline state machine
    // ---------------------------------------------------------------------------
    enum class State
    {
        Opening,       ///< Trying to open V4L2Source
        Streaming,     ///< Actively capturing and submitting frames to encoder
        SourceChanged, ///< V4L2_EVENT_SOURCE_CHANGE received; resolve new resolution
        EncoderFault,  ///< Encoder stopped unexpectedly; rebuild before next open
        Reconnecting,  ///< Waiting reconnect_delay_ms before next open attempt
        Stopping,      ///< Terminal: exit loop thread
    };

    // Each handler executes one state and returns the next state.
    State on_opening(int width, int height, int &retry_count);
    State on_streaming();
    State on_source_changed(int &width, int &height, int &retry_count);
    State on_encoder_fault(int width, int height);
    State on_reconnecting(int &retry_count);

    // Pipeline primitives used by the state handlers.
    bool open_source(int width, int height);
    void close_source();
    bool rebuild_encoder(int width, int height);
    bool handle_source_change(int &width, int &height);
    bool wait_reconnect(int &retry_count);

    void loop_thread_func();

    EncMgrConfig m_cfg;

    std::unique_ptr<RTEncoderV4L2> m_encoder; ///< Created in start(), destroyed on loop exit
    std::shared_ptr<V4L2Source> m_source;     ///< Opened/closed each capture session

    std::thread m_loop_thread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_enc_mutex; ///< Protects m_encoder for external callers
};

#endif // ENC_MGR_H
