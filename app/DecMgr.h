#ifndef DEC_MGR_H
#define DEC_MGR_H

#include "VideoTypes.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

class RTDecoder;
class DRMDisplay;
class ReliableUDP;

struct AL_TBuffer;
struct AL_TInfoDecode;

/**
 * @brief Configuration bundle for DecMgr initialization.
 *
 * Only standard C++ types are exposed here; internal SDK and DRM types are
 * hidden inside DecMgr.cpp.
 *
 * @par Example
 * @code
 *   DecMgrConfig cfg;
 *   cfg.codec = VideoCodec::HEVC;
 *   cfg.drm_device = "/dev/dri/card0";
 *   cfg.desired_width  = 1920;
 *   cfg.desired_height = 1080;
 *   cfg.udp_local_port = 5004;
 *
 *   DecMgr mgr(cfg);
 * @endcode
 */
struct DecMgrConfig
{
    VideoCodec codec = VideoCodec::HEVC; ///< Video codec
    bool low_delay_mode =
        false; ///< LLP2 mode: bUseEarlyCallback on decoder + llp2_mode on display (LLP1 always active)
    uint32_t input_buffer_size = 4u * 1024u * 1024u; ///< Bytes per input stream buffer
    std::string dec_dev = "/dev/allegroDecodeIP";    ///< VCU decoder device
    std::string drm_device = "/dev/dri/card0";       ///< DRM device node
    int desired_width = 0;                           ///< Preferred display width  (0 = no preference)
    int desired_height = 0;                          ///< Preferred display height (0 = no preference)
    int desired_refresh = 0;                         ///< Preferred refresh rate in Hz (0 = auto)
    uint16_t udp_local_port = 0;                     ///< Local UDP port to receive bitstream on (required, > 0)
    std::string udp_reply_addr;                      ///< Encoder address for RTT probes (optional)
    uint16_t udp_reply_port = 0;                     ///< Encoder UDP port for RTT probes (0 = disabled)
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
 *   cfg.codec = VideoCodec::HEVC;
 *   cfg.drm_device = "/dev/dri/card0";
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
 * - Drains display and releases decoder-owned framebuffer imports
 * - Automatically rebuilds decoder in-place
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
 * - fps() may be called from any thread at any time.
 * - Internal callbacks execute on SDK thread (on_decoded_frame) and DRM event thread (return_frame).
 * - push_stream() is private; data enters the pipeline only via the internal UDP receiver.
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

    double fps() const;
    double recv_rate() const;
    double lost_rate() const;
    std::string queue_stats_text() const;
    int64_t rtt_ms() const;
    int64_t offset_ms() const;

  private:
    bool push_stream(const void *data, size_t size, StreamFlags flags = StreamFlags::Unknown);
    void on_decoded_frame(AL_TBuffer *frame, const AL_TInfoDecode &info);
    void return_frame(AL_TBuffer *frame);
    bool create_decoder();
    bool do_rebuild();

  private:
    DecMgrConfig m_cfg;
    mutable std::mutex m_dec_mutex;
    std::unique_ptr<RTDecoder> m_decoder;
    std::unique_ptr<DRMDisplay> m_display;
    std::shared_ptr<ReliableUDP> m_receiver;
    std::atomic<bool> m_running{false};
};

#endif // DEC_MGR_H
