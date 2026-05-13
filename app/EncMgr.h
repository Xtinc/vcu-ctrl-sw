#ifndef ENC_MGR_H
#define ENC_MGR_H

#include "RTEncoder.h"
#include "V4L2Source.h"

#include <atomic>
#include <memory>
#include <thread>
#include <utility>

/**
 * @brief EncMgrConfig holds all configuration needed to create an EncMgr instance.
 */
struct EncMgrConfig
{
    EncoderConfig enc;             ///< Encoder parameters (width/height = actual capture resolution)
    std::string v4l2_dev;          ///< V4L2 capture device path, e.g. "/dev/video0"
    std::string sync_dev;          ///< Xilinx sync device path (empty = disabled)
    int reconnect_delay_ms = 2000; ///< Delay between reconnection attempts in milliseconds
    int max_reconnect_tries = -1;  ///< Max consecutive reconnect attempts (-1 = unlimited)
};

/**
 * @brief EncMgr is the top-level encoding manager.
 *
 * It owns both the V4L2 capture source and the hardware encoder, coordinates their
 * lifecycle, and handles:
 *   - Source signal loss and reconnection (with configurable retry limit and delay)
 *   - Source resolution changes (V4L2_EVENT_SOURCE_CHANGE): old V4L2Source is
 *     destroyed, encoder is reconfigured via set_resolution(), and a new V4L2Source
 *     is constructed at the new resolution
 *   - Encoder errors: flush + restart on the next reconnect cycle
 *
 * The encoder (RTEncoderV4L2) is created once and lives for the entire EncMgr
 * lifetime; only V4L2Source is torn down and rebuilt on each reconnect or
 * resolution change. RTEncoderBase::init_settings() always programs the hardware
 * channel at kMaxWidth × kMaxHeight, so dynamic set_resolution() is safe.
 *
 * All state is fully internal. The public interface is minimal:
 *   - start() / stop()
 *   - Dynamic encoder control: set_bitrate(), set_framerate(), request_IDR(), fps()
 *
 * Threading model:
 *   - start() spawns a single loop thread that drives the entire encode pipeline.
 *   - stop() signals the thread and blocks until it exits (including encoder flush).
 *   - All dynamic control methods (set_bitrate etc.) are thread-safe: they acquire
 *     m_enc_mutex before touching m_encoder, which also synchronizes with the loop
 *     thread's final flush+reset sequence.
 *
 * Typical usage:
 * @code
 *   EncMgrConfig cfg;
 *   cfg.enc.width = 1920; cfg.enc.height = 1080;
 *   cfg.v4l2_dev = "/dev/video0";
 *   cfg.enc.target_bitrate = 8'000'000;
 *
 *   EncMgr mgr(cfg, [](const uint8_t *data, size_t sz){ write_to_output(data, sz); });
 *   mgr.start();
 *   // ... application runs ...
 *   mgr.stop();
 * @endcode
 */
class EncMgr
{
  public:
    using EncodedFrameCallback = RTEncoderBase::EncodedFrameCallback;

    /**
     * @brief Construct an EncMgr. Does not start encoding; call start().
     * @param cfg    Configuration for encoder and capture device.
     * @param output_cb Callback invoked on the encoder SDK thread for each encoded AU.
     * @throw std::invalid_argument if cfg.v4l2_dev is empty or output_cb is null.
     */
    explicit EncMgr(EncMgrConfig cfg, EncodedFrameCallback output_cb);

    /**
     * @brief Destructor. Calls stop() if still running.
     */
    ~EncMgr();

    EncMgr(const EncMgr &) = delete;
    EncMgr &operator=(const EncMgr &) = delete;
    EncMgr(EncMgr &&) = delete;
    EncMgr &operator=(EncMgr &&) = delete;

    /**
     * @brief Start the encode pipeline.
     *
     * Creates the RTEncoderV4L2 instance, then spawns the internal loop thread
     * which opens the V4L2 device, imports DMA buffers, and begins streaming.
     *
     * Non-blocking: returns immediately after the thread is launched.
     *
     * @return true on success, false if already running or encoder creation fails.
     */
    bool start();

    /**
     * @brief Stop the encode pipeline.
     *
     * Signals the loop thread to exit, waits for it to complete (including the
     * encoder EOS flush), then destroys the encoder.
     *
     * Blocking: maximum wait ≈ V4L2 dequeue timeout (1 s) + encoder flush timeout (5 s).
     * Safe to call from any thread, including after a previous stop().
     */
    void stop();

    /**
     * @brief Dynamically update the target (and optionally peak) bitrate.
     * @param target Target bitrate in bps. Must be > 0.
     * @param max    VBR peak bitrate in bps. 0 = same as target (CBR).
     * @return true on success, false if encoder is not running or SDK rejects the value.
     */
    bool set_bitrate(uint32_t target, uint32_t max = 0);

    /**
     * @brief Dynamically update the encode frame rate.
     * @param fps Frame rate numerator. Must be > 0.
     * @param clk Frame rate denominator. Must be > 0.
     * @return true on success, false if encoder is not running or SDK rejects the value.
     */
    bool set_framerate(uint32_t fps, uint32_t clk = 1000);

    /**
     * @brief Force the encoder to insert an IDR frame at the next opportunity.
     */
    void request_IDR();

    /**
     * @brief Return the latest exponential-moving-average throughput statistics.
     * @return {fps, bitrate_bps}. Values are 0.0 until at least 100 frames have been encoded.
     */
    std::pair<double, double> fps() const;

  private:
    enum class LoopExitReason
    {
        Stopped,
        SourceChanged,
        DeviceError
    };

    void loop_thread_func();
    bool build_pipeline(std::shared_ptr<V4L2Source> &out_src, int width, int height);
    LoopExitReason run_capture_loop(V4L2Source &src);
    void shutdown_pipeline(std::shared_ptr<V4L2Source> &src);
    bool try_apply_resolution_change(int &current_w, int &current_h);
    bool wait_before_reconnect(int &retry_count);
    // Flush, destroy, and recreate m_encoder at the given resolution.
    // Returns false (and leaves m_encoder null) on unrecoverable failure.
    bool rebuild_encoder(int width, int height);

    EncMgrConfig m_cfg;
    EncodedFrameCallback m_output_cb;

    std::unique_ptr<RTEncoderV4L2> m_encoder;
    std::thread m_loop_thread;

    std::atomic<bool> m_running{false};
    mutable std::mutex m_enc_mutex; ///< Protects m_encoder pointer from concurrent access
};

#endif // ENC_MGR_H
