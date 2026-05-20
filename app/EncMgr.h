#ifndef ENC_MGR_H
#define ENC_MGR_H

#include "RTEncoder.h"
#include "V4L2Source.h"

/**
 * @brief EncMgrConfig holds all configuration needed to create an EncMgr instance.
 */
struct EncMgrConfig
{
    EncoderConfig enc;       ///< Encoder parameters (width/height are initial fallback values)
    std::string v4l2_dev;    ///< V4L2 capture device path, e.g. "/dev/video0" (required)
    std::string v4l2_subdev; ///< V4L2 sub-device for source detection and events, e.g. "/dev/v4l-subdev0" (required)
    std::string sync_dev;    ///< Xilinx sync device path (empty = disabled)
    int source_check_interval_ms = 2000; ///< Interval for checking source presence in WaitingSource state
};

/**
 * @brief EncMgr is the top-level encoding manager.
 *
 * Owns both the V4L2 capture source (m_source) and the hardware encoder (m_encoder),
 * and coordinates their lifecycle via an explicit state machine:
 *
 *   Opening:
 *     query_source ok          -> Streaming
 *     no source detected       -> WaitingSource
 *     open_source fail         -> WaitingSource (wait for user to fix)
 *     stop()                   -> Stopping
 *   Streaming:
 *     dqueue/submit loop active
 *     stop() / normal exit     -> Stopping
 *     V4L2 device error        -> WaitingSource
 *     encoder fault            -> EncoderFault
 *     V4L2_EVENT_SOURCE_CHANGE -> SourceChanged
 *   SourceChanged:
 *     set_resolution() ok      -> Opening  (no delay)
 *     set_resolution() fail    -> rebuild_encoder() -> Opening  (no delay)
 *     rebuild_encoder() fail   -> Stopping
 *     stop()                   -> Stopping
 *   EncoderFault:
 *     rebuild_encoder() ok     -> Opening  (no delay)
 *     rebuild_encoder() fail   -> Stopping
 *     stop()                   -> Stopping
 *   WaitingSource:
 *     Wait on condition variable with timeout, periodically check probe_subdev_format()
 *     source detected          -> Opening  (no delay)
 *     stop()                   -> Stopping
 *   Stopping:
 *     terminal state; flush encoder EOS, destroy encoder, exit loop thread
 *
 * The encoder is created once in start() and kept alive across state transitions;
 * only m_source is torn down and rebuilt each time. m_encoder is flushed (EOS) once
 * at final stop, not between sessions, so the encode session remains continuous.
 *
 * Requirements:
 *   - v4l2_subdev must be configured (HDMI/SDI/CSI capture cards with sub-device)
 *   - USB cameras without sub-devices are NOT supported
 *   - Driver support for format probing and source-change events
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
    enum class State
    {
        Opening,       ///< Trying to open V4L2Source
        Streaming,     ///< Actively capturing and submitting frames to encoder
        SourceChanged, ///< V4L2_EVENT_SOURCE_CHANGE received; resolve new resolution
        EncoderFault,  ///< Encoder stopped unexpectedly; rebuild before next open
        WaitingSource, ///< No source detected; wait on condition variable with periodic check
        Stopping,      ///< Terminal: exit loop thread
    };

  public:
    /**
     * @brief Construct an EncMgr. Does not start encoding; call start().
     * @param cfg       Configuration for encoder and capture device.
     * @throw std::invalid_argument if cfg.v4l2_dev or cfg.v4l2_subdev is empty.
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
    State on_opening(int &width, int &height);
    State on_streaming();
    State on_source_changed(int &width, int &height);
    State on_encoder_fault(int width, int height);
    State on_waiting_source();
    static const char *state_to_cstr(State s);

    bool open_source(int width, int height);
    void close_source();
    bool rebuild_encoder(int width, int height);
    bool ensure_encoder_at(int width, int height);
    bool handle_source_change(int &width, int &height);
    bool query_source_resolution(int &width, int &height) const;

    void loop_thread_func();

  private:
    EncMgrConfig m_cfg;

    std::unique_ptr<RTEncoderV4L2> m_encoder;
    std::shared_ptr<V4L2Source> m_source;

    std::thread m_loop_thread;
    std::atomic<bool> m_running{false};
    mutable std::mutex m_enc_mutex;

    std::mutex m_wait_mutex;
    std::condition_variable m_wait_cv;
};

#endif // ENC_MGR_H
