#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_decode/lib_decode.h"
}

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

/**
 * @brief Configuration parameters for DRMDisplay.
 */
struct DRMDisplayConfig
{
    std::string drm_device = "/dev/dri/card0"; ///< DRM device node path.
    int connector_id = -1;                     ///< KMS connector ID (-1 = auto-detect first connected).
    int crtc_id = -1;                          ///< KMS CRTC ID (-1 = auto-detect from connector).
    int plane_id = -1;                         ///< KMS plane ID (-1 = auto-detect overlay or primary plane).
    bool enable_vsync = true;                  ///< Wait for page-flip event before releasing frame.
    bool force_mode_set = false;               ///< Force drmModeSetCrtc even if a mode is already active.
};

/**
 * @brief Zero-copy DRM/KMS display for Xilinx ZYNQ VCU decoded frames.
 *
 * Accepts decoded frames from RTDecoder via show() and presents them on screen
 * using DRM PRIME DMA-buf import (zero CPU copy).  Each unique AL_TBuffer* is
 * imported once and cached; subsequent frames reuse the cached framebuffer.
 *
 * Atomic modesetting is used when available; falls back to drmModeSetPlane.
 * enable_vsync=true waits for the page-flip event (atomic) or drmWaitVBlank
 * (legacy) before releasing the previous frame.
 *
 * @note Non-copyable, non-movable.
 * @note The DecodedFrameCallback must call RTDecoder::return_display_frame() when done.
 * @throws std::runtime_error  On DRM device open or resource enumeration failure.
 */
class DRMDisplay
{
  public:
    /**
     * @brief Callback invoked when the display is done with a frame.
     * Typically wraps RTDecoder::return_display_frame().
     */
    using FrameReleaseCallback = std::function<void(AL_TBuffer *)>;

    /** Construct and start the DRM display pipeline.
     *  @throws std::invalid_argument if release_cb is null.
     *  @throws std::runtime_error on DRM open/enumeration failure. */
    explicit DRMDisplay(const DRMDisplayConfig &cfg, FrameReleaseCallback release_cb);

    /** Drain the display thread and release the last held frame via FrameReleaseCallback.
     *  If not called, the destructor skips the callback. Idempotent. */
    ~DRMDisplay();

    DRMDisplay(const DRMDisplay &) = delete;
    DRMDisplay &operator=(const DRMDisplay &) = delete;
    DRMDisplay(DRMDisplay &&) = delete;
    DRMDisplay &operator=(DRMDisplay &&) = delete;

    /** Drain the display thread and release the last held frame via FrameReleaseCallback.
     *  Must be called while the decoder is still alive. Idempotent. */
    void stop();

    /** Submit a decoded frame for display (call from RTDecoder::DecodedFrameCallback). */
    void show(AL_TBuffer *frame, const AL_TInfoDecode &info);

  private:
    // Internal frame representations
    struct PendingFrame
    {
        AL_TBuffer *buf = nullptr;
        uint32_t w = 0;
        uint32_t h = 0;
    };

    struct Frame
    {
        AL_TBuffer *buf = nullptr;
        uint32_t fb_id = 0;
    };

    /// Cached DRM framebuffer (fb_id + GEM handles) for a decoder buffer.
    struct CachedFB
    {
        uint32_t fb_id = 0;
        std::array<uint32_t, 4> gem_handles{0, 0, 0, 0};
        int num_gem = 0;
    };

    static constexpr size_t MAX_PENDING_FRAMES = 2; ///< Bounded queue depth.

    // DRM plane property IDs for atomic modesetting
    struct PlaneProps
    {
        uint32_t fb_id = 0;
        uint32_t crtc_id = 0;
        uint32_t crtc_x = 0;
        uint32_t crtc_y = 0;
        uint32_t crtc_w = 0;
        uint32_t crtc_h = 0;
        uint32_t src_x = 0;
        uint32_t src_y = 0;
        uint32_t src_w = 0;
        uint32_t src_h = 0;
    };

    // Private helpers
    void init_drm();
    void close_drm();
    void do_stop(bool call_release);
    void submit_frame(PendingFrame pf);
    void display_thread_fn();
    void free_frame(Frame &f, bool call_release);
    void wait_flip_event(bool &flip_done);
    void wait_vblank();
    bool create_fb(AL_TBuffer *buf, uint32_t w, uint32_t h, uint32_t &out_fb_id);

    // DRM state (written once in init_drm, then read-only)
    DRMDisplayConfig m_cfg;
    FrameReleaseCallback m_release_cb;

    int m_drm_fd{-1};
    uint32_t m_conn_id{0};
    uint32_t m_crtc_id{0};
    uint32_t m_plane_id{0};
    int m_pipe{0};
    bool m_first_frame{true};
    bool m_atomic{false};
    PlaneProps m_plane_props{};

    // Framebuffer cache: decoder buffer pointer → imported DRM framebuffer
    std::map<AL_TBuffer *, CachedFB> m_fb_cache;
    std::mutex m_cache_mutex;

    // Display thread synchronisation
    Frame m_held; ///< Frame currently on screen (display thread only).

    std::queue<PendingFrame> m_pending_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv_display;  ///< Wakes display thread on new frame or stop.
    std::condition_variable m_cv_producer; ///< Wakes show() when queue has space.

    std::atomic<bool> m_stopped{false};
    std::thread m_thread;
};

#endif // DRM_DISPLAY_H
