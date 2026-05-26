#ifndef REALTIME_ENCODER_H
#define REALTIME_ENCODER_H

#include "DMAFd.h"
#include "MemMgr.h"
#include <atomic>
#include <chrono>
#include <unordered_map>

extern "C"
{
#include "lib_common_enc/EncChanParam.h"
#include "lib_common_enc/Settings.h"
#include "lib_encode/lib_encoder.h"
}

struct EncoderConfig
{
    AL_EProfile profile = AL_PROFILE_HEVC_MAIN;   // AVC/HEVC Profile
    uint8_t level = 51;                           // Level (e.g., H.265 Level 5.1)
    uint8_t tier = 0;                             // 0 = Main Tier, 1 = High Tier (HEVC only)
    uint16_t width = 3840;                        // Frame width
    uint16_t height = 2160;                       // Frame height
    AL_EChromaMode chroma_mode = AL_CHROMA_4_2_0; // Chroma sampling format
    uint8_t bit_depth = 8;                        // Bit depth (8 or 10)
    AL_ERateCtrlMode rc_mode = AL_RC_CBR;         // Rate control mode
    uint32_t target_bitrate = 25 * 1000 * 1000;   // Target bitrate in bps
    uint32_t max_bitrate = 0;                     // VBR peak bitrate in bps, 0 = same as target
    int16_t initial_qp = 35;                      // Initial QP (fixed QP in CQP mode)
    uint16_t framerate = 60;                      // Frame rate numerator
    uint16_t clk_ratio = 1000;                    // Frame rate denominator, final frame rate = framerate*1000/clk_ratio
    uint16_t gop_length = 60;                     // GOP length (number of frames between two IDR/I frames)
    uint32_t freq_idr = 240;                      // IDR forced insertion frequency (0 = only GOP first frame)
    uint8_t num_b = 2;                            // Number of B-frames in GOP (ignored if low_delay_mode is true)
    uint32_t num_src_bufs = 4;                    // Number of input source frame buffers
    uint32_t num_stream_bufs = 4;                 // Number of output stream buffers
    std::string enc_dev_path = "/dev/allegroIP";  // Encoder device node (e.g., "/dev/allegroIP")
    std::string dma_dev_path = "/dev/dmaproxy";   // DMAProxy device node (e.g., "/dev/dmaproxy")
    bool low_delay_mode = false; // true = low-latency P-frame GOP (no B-frames, minimal encode/decode latency)
};

enum class SourceMode
{
    FILE,
    V4L2
};

/**
 * @brief RTEncoderBase is a hardware-accelerated video encoder wrapper for the Allegro DVT VCU SDK.
 *
 * This class manages the full encoder lifecycle: SDK initialization, DMA allocator and MCU scheduler
 * creation, source/stream buffer pool management, GOP and rate-control parameter application, and
 * thread-safe state transitions. Concrete subclasses (RTEncoder<FILE> and RTEncoder<V4L2>) provide
 * source-buffer acquisition and release strategies.
 *
 * Lifecycle state machine (single atomic state):
 *
 *   RUNNING:
 *     submit_source_buffer  -> submits frame, stays in RUNNING
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
 *     terminal state set by destructor before AL_Encoder_Destroy;
 *     all stream-buffer return calls are suppressed in this state
 *
 * Thread safety:
 *   - submit_source_buffer() / flush() must be called from a single producer thread.
 *   - SDK callbacks (on_encoded_frame) execute on an internal SDK thread.
 *   - m_state is std::atomic; m_cfg is not internally locked (callers must serialize).
 *
 * Typical usage (FILE mode):
 * @code
 *   EncoderConfig cfg;
 *   cfg.width = 1920; cfg.height = 1080;
 *   RTEncoderFile enc(cfg, [](const uint8_t *data, size_t sz){ write(data, sz); });
 *
 *   while (have_frames) {
 *       auto *buf = enc.acquire_source_buffer();
 *       fill(buf);
 *       enc.submit_source_buffer(buf);
 *   }
 *   enc.flush(); // blocks until EOS or timeout
 * @endcode
 */
/// Callback invoked for each encoded NAL unit / AU on the SDK thread.
using EncodedFrameCallback = std::function<void(const uint8_t *pData, size_t size, bool eof)>;

class RTEncoderBase
{
  public:
    virtual ~RTEncoderBase();

    RTEncoderBase(const RTEncoderBase &) = delete;
    RTEncoderBase &operator=(const RTEncoderBase &) = delete;
    RTEncoderBase(RTEncoderBase &&) = delete;
    RTEncoderBase &operator=(RTEncoderBase &&) = delete;

    /**
     * @brief Flush the encoder: queue EOS and block until the EOS callback is received.
     *
     * Transitions state from Running -> Flushing. Waits up to 5 seconds for the SDK
     * to drain all pending frames and deliver the EOS callback. If already flushed or
     * stopped, returns true immediately.
     *
     * Never throws. Safe to call after a network disconnect or any abnormal condition.
     *
     * @return @c true  EOS received — all frames have been delivered.
     *         @c false EOS could not be queued, or the wait timed out.
     */
    bool flush();

    /** @return true if the encoder is still in Running state and accepts new frames. */
    bool is_running() const;

    /**
     * @brief Force the encoder to insert an IDR frame at the next opportunity.
     */
    void request_IDR();

    /**
     * @brief Dynamically update the target (and optionally peak) bitrate.
     * @param uTargetBitRate Target bitrate in bps. Must be > 0.
     * @param uMaxBitRate    VBR peak bitrate in bps. 0 = same as target (CBR).
     * @return true on success, false on invalid arguments or SDK rejection.
     */
    bool set_bitrate(uint32_t uTargetBitRate, uint32_t uMaxBitRate = 0);

    /**
     * @brief Dynamically update the encode frame rate.
     * @param uFrameRate  Frame rate numerator. Must be > 0.
     * @param uClkRatio   Frame rate denominator (final fps = uFrameRate * 1000 / uClkRatio). Must be > 0.
     * @return true on success, false on invalid arguments or SDK rejection.
     */
    bool set_framerate(uint32_t uFrameRate, uint32_t uClkRatio = 1000);

    /**
     * @brief Dynamically change the active encoding resolution.
     *
     * Must not exceed the maximum dimensions (kMaxWidth × kMaxHeight) used at encoder creation.
     *
     * @param uWidth  New frame width in pixels. Must be > 0.
     * @param uHeight New frame height in pixels. Must be > 0.
     * @return true on success, false on out-of-range or SDK rejection.
     */
    bool set_resolution(uint32_t uWidth, uint32_t uHeight);

    /** @return The source buffer FourCC code expected by the encoder. */
    TFourCC src_fourCC() const;

    /** @return The source bit depth (8 or 10). */
    uint8_t src_bitdepth() const;

    /** @return The source chroma sampling format. */
    AL_EChromaMode src_chroma() const;

    /** @return The current encoding resolution. Caller must ensure serialized access. */
    AL_TDimension src_resolution() const;

    /**
     * @brief Return the latest exponential-moving-average throughput statistics.
     * @return {fps, bitrate_bps} pair. Values are 0.0 until at least 100 frames have been encoded.
     */
    std::pair<double, double> fps() const;

  protected:
    /**
     * @brief Construct the base encoder: initialize SDK, allocator, scheduler, buffer pools, and encoder handle.
     * @param cfg Configuration parameters.
     * @param cb  Encoded data callback. Must not be null.
     * @throw std::runtime_error on any initialization failure. All partially allocated resources are released.
     */
    explicit RTEncoderBase(const EncoderConfig &cfg, EncodedFrameCallback cb);
    void signal_done();

  private:
    static void sdk_callback(void *pUserParam, AL_TBuffer *pStream, AL_TBuffer const *pSrc, int iLayerID);
    void on_encoded_frame(AL_TBuffer *pStream, AL_TBuffer const *pSrc);
    void update_frame_rate();

    void init_settings(AL_TEncSettings &settings) const;
    void init_source_buf_pool();
    void init_stream_buf_pool();
    void push_stream_buffers();
    virtual void release_sources(AL_TBuffer const *pSrc) = 0;

  protected:
    mutable std::mutex m_fps_mutex;
    double m_fps;
    double m_bitrate;
    uint32_t m_frame_count;
    uint32_t m_bytes_count;
    std::chrono::steady_clock::time_point m_fps_last_time;

    EncoderConfig m_cfg;
    EncodedFrameCallback m_callback;

    DMAProxy m_dma_proxy;
    AL_TAllocator *m_pAllocator;
    AL_IEncScheduler *m_pScheduler;
    AL_HEncoder m_hEnc;

    std::unique_ptr<PixMapBufPool> m_source_buf_pool;
    std::unique_ptr<GenericBufPool> m_stream_buf_pool;

    AL_TPicFormat m_pic_format;
    TFourCC m_src_fourcc;

    enum class State : uint8_t
    {
        Running = 0,  ///< Normal encoding: accepts frame input, allows stream buffer return
        Flushing = 1, ///< Draining: no new input accepted, waiting for EOS callback
        Done = 2,     ///< Terminal: EOS received or fatal error; wakes flush() waiter
        Stopping = 3, ///< Destructor guard: stream buffer return suppressed (AL_Encoder_Destroy imminent)
    };
    std::atomic<State> m_state{State::Running};

    std::mutex m_eos_mutex;
    std::condition_variable m_eos_cond;
    bool m_lib_initialized;
};

template <SourceMode mode> class RTEncoder;
using RTEncoderFile = RTEncoder<SourceMode::FILE>;
using RTEncoderV4L2 = RTEncoder<SourceMode::V4L2>;

/**
 * @brief File/software source encoder: the caller owns the source buffer lifecycle.
 *
 * The caller acquires a source buffer via acquire_source_buffer(), fills it with raw YUV data,
 * then hands it back via submit_source_buffer(). Ownership transfers to the encoder on submit;
 * the buffer is automatically returned to the pool via the source-release callback.
 */
template <> class RTEncoder<SourceMode::FILE> : public RTEncoderBase
{
  public:
    /**
     * @brief Construct a FILE-mode encoder.
     * @param cfg Configuration parameters.
     * @param cb  Encoded data callback.
     * @throw std::runtime_error on initialization failure.
     */
    RTEncoder(const EncoderConfig &cfg, EncodedFrameCallback cb);
    ~RTEncoder() override = default;

    /**
     * @brief Acquire an idle source buffer from the pool.
     *
     * Blocks until a buffer becomes available (or the pool is decommitted on flush).
     *
     * @return Non-null AL_TBuffer* ready for writing, or nullptr if stopped/error.
     */
    AL_TBuffer *acquire_source_buffer();

    /**
     * @brief Submit a filled source buffer to the encoder for encoding.
     *
     * Ownership transfers to the encoder; do not access @p pBuf after this call.
     * The RAII guard inside this function unrefs the buffer regardless of success.
     *
     * @param pBuf Buffer previously returned by acquire_source_buffer(). Must not be null.
     * @return true if the frame was accepted, false on encoder error or invalid state.
     */
    bool submit_source_buffer(AL_TBuffer *pBuf);

  private:
    void release_sources(AL_TBuffer const *pSrc) override;
};

/// Callback invoked on the SDK thread when the encoder has finished reading a source buffer.
using SourceReleaseCallback = std::function<void(AL_TBuffer const *pSrc)>;

/**
 * @brief V4L2 DMABUF source encoder: zero-copy pipeline from V4L2Source to VCU encoder.
 *
 * DMA buffers are pre-allocated by the encoder pool, exported as DMA-BUF file descriptors
 * via acquire_dma_buffers(), and imported by V4L2Source. After the V4L2 device fills a buffer
 * and it is dequeued, the caller submits it directly to the encoder via submit_source_buffer().
 * When the SDK is done with the buffer the registered SourceReleaseCallback fires so the caller
 * can requeue it back to V4L2.
 */
template <> class RTEncoder<SourceMode::V4L2> : public RTEncoderBase
{
  public:
    /**
     * @brief Construct a V4L2 DMABUF-mode encoder.
     * @param cfg Configuration parameters.
     * @param cb  Encoded data callback.
     * @throw std::runtime_error on initialization failure.
     */
    RTEncoder(const EncoderConfig &cfg, EncodedFrameCallback cb);
    ~RTEncoder() override;

    /**
     * @brief Register the callback invoked when the SDK releases a source buffer.
     *
     * The callback is called from the SDK internal thread. It should requeue the buffer
     * to V4L2Source. Thread-safe; may be called at any time before or during streaming.
     *
     * @param releaseCb Callback receiving the AL_TBuffer* identity of the released buffer.
     */
    void set_release_callback(SourceReleaseCallback releaseCb);

    /**
     * @brief Acquire @p count source buffers from the pool and return their DMA-BUF descriptors.
     *
     * Each returned DMAFd carries an AL_TBuffer reference. The descriptors are intended to be
     * imported by V4L2Source::import_fds(). The DMAFd destructor unrefs the buffer.
     *
     * @param count Number of DMA buffer descriptors to acquire. Must not exceed pool capacity.
     * @return DMAFdArray of the requested descriptors, or empty on failure.
     */
    DMAFdArray acquire_dma_buffers(unsigned int count);

    /**
     * @brief Submit a DMA source buffer (dequeued from V4L2) to the encoder.
     *
     * Does not transfer ownership; the buffer will be returned via SourceReleaseCallback.
     *
     * @param pBuf AL_TBuffer* identity obtained from a V4L2 dequeue operation. Must not be null.
     * @return true if accepted, false on invalid state or SDK error.
     */
    bool submit_source_buffer(AL_TBuffer *pBuf);

  private:
    void release_sources(AL_TBuffer const *pSrc) override;
    std::mutex m_mutex;
    SourceReleaseCallback m_release_cb;
};

#endif // REALTIME_ENCODER_H
