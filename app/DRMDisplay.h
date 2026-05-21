#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

#include "lib_network/clock_wait.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

/**
 * @brief Configuration parameters for DRMDisplayBase and its subclasses.
 */
struct DRMDisplayConfig
{
    std::string drm_device = "/dev/dri/card0"; ///< DRM device node path.
    int connector_id = -1;                     ///< KMS connector ID (-1 = auto-detect).
    int crtc_id      = -1;                     ///< KMS CRTC ID (-1 = auto-detect).
    int plane_id     = -1;                     ///< KMS plane ID (-1 = auto-detect overlay/primary).

    /// How far before the predicted vblank to submit the atomic commit.
    /// Smaller = lower latency but higher risk of missing the vblank window.
    /// Default: 1 ms — safe for most hardware.
    std::chrono::nanoseconds submit_lead_time{1'000'000LL};
};

/**
 * @brief Abstract base: DRM/KMS double-buffer + Atomic API event loop.
 *
 * Implements the complete event thread, double-buffer state machine (4 states × 2 slots),
 * vblank-timed Atomic API submission (ClockEntry), and page-flip event handling.
 *
 * Double-buffer state machine:
 *   FREE ──show()──► PENDING ──schedule_flip()──► SCANNING
 *    ▲   (latest-wins: old PENDING released)          │
 *    │                                                │ drmModeAtomicCommit
 *    │                               SCANNING ──► RELEASING
 *    └─────────────── on_flip_done: RELEASING ──► FREE + release_cb
 *
 * Thread model:
 * • Producer thread: show() — places frame in PENDING, never blocks.
 * • Event thread: all DRM API calls — drmModeAtomicCommit, poll, drmHandleEvent.
 *
 * Subclasses call the protected submit() after resolving the DRM framebuffer ID.
 *
 * @note Requires DRM_CLIENT_CAP_ATOMIC (throws std::runtime_error otherwise).
 * @note Non-copyable, non-movable.
 */
class DRMDisplayBase
{
  public:
    using FrameReleaseCallback = std::function<void(void *)>;

    /** @throws std::invalid_argument  if release_cb is null.
     *  @throws std::runtime_error     on DRM open / Atomic cap / resource enumeration failure. */
    explicit DRMDisplayBase(const DRMDisplayConfig &cfg, FrameReleaseCallback release_cb);

    /** Stops the event thread and releases all held frames. */
    virtual ~DRMDisplayBase();

    DRMDisplayBase(const DRMDisplayBase &) = delete;
    DRMDisplayBase &operator=(const DRMDisplayBase &) = delete;
    DRMDisplayBase(DRMDisplayBase &&)                 = delete;
    DRMDisplayBase &operator=(DRMDisplayBase &&)      = delete;

    /** Drain the event thread and release all held frames via FrameReleaseCallback. Idempotent. */
    void stop();

  protected:
    /** Returns the open DRM file descriptor. Valid between construction and destruction. */
    int drm_fd() const { return m_drm_fd; }

    /** Enqueue a pre-resolved frame into the display pipeline. Never blocks.
     *  key   : opaque handle returned to FrameReleaseCallback when the frame is retired.
     *  fb_id : registered DRM framebuffer (caller is responsible for its lifetime).
     */
    void submit(void *key, uint32_t fb_id, uint32_t w, uint32_t h);

  private:
    // ── Slot state machine ────────────────────────────────────────────────
    enum class SlotState { FREE, PENDING, SCANNING, RELEASING };

    struct Slot
    {
        void    *buf   = nullptr;
        uint32_t fb_id = 0;
        uint32_t w     = 0;
        uint32_t h     = 0;
        SlotState state = SlotState::FREE;
    };

    struct PlaneProps
    {
        uint32_t fb_id  = 0, crtc_id = 0;
        uint32_t crtc_x = 0, crtc_y  = 0, crtc_w = 0, crtc_h = 0;
        uint32_t src_x  = 0, src_y   = 0, src_w  = 0, src_h  = 0;
    };
    struct CrtcProps { uint32_t active = 0, mode_id = 0; };
    struct ConnProps { uint32_t crtc_id = 0; };

    void init_drm();
    void close_drm();

    bool do_modeset_locked(uint32_t fb_id, uint32_t w, uint32_t h);
    bool schedule_flip_locked();
    Slot *slot_by_state_locked(SlotState s);

    void event_thread_fn();
    static void on_page_flip_cb(int fd, unsigned seq, unsigned tv_sec,
                                unsigned tv_usec, void *user_data);
    void on_flip_done(unsigned tv_sec, unsigned tv_usec);

    DRMDisplayConfig     m_cfg;
    FrameReleaseCallback m_release_cb;

    int      m_drm_fd   {-1};
    uint32_t m_conn_id  {0};
    uint32_t m_crtc_id  {0};
    uint32_t m_plane_id {0};
    int      m_pipe     {0};

    uint32_t   m_mode_blob_id {0};
    bool       m_modeset_done {false};
    PlaneProps m_plane_props  {};
    CrtcProps  m_crtc_props   {};
    ConnProps  m_conn_props   {};

    Slot                    m_slots[2];
    bool                    m_in_flight {false}; ///< True while an atomic commit is outstanding.
    std::mutex              m_mutex;             ///< Guards m_slots and m_in_flight.
    std::condition_variable m_cv;

    // ── Vblank timing (event thread only — no lock needed) ────────────────
    using SteadyClock = std::chrono::steady_clock;
    using TimePoint   = SteadyClock::time_point;
    using Nanos       = std::chrono::nanoseconds;

    TimePoint m_last_flip_tp {};
    Nanos     m_frame_ns     {16'666'666LL}; ///< EMA of vblank interval; default = 60 fps.
    int       m_in_flight_retries {0};      ///< Consecutive poll timeouts while in-flight (watchdog counter).

    // ── Thread management ─────────────────────────────────────────────────
    std::atomic<bool> m_stopped {false};
    std::thread       m_event_thread;
    ClockEntry        m_submit_timer;
};

// ── DRMDisplay: Allegro VCU DMA-buf zero-copy ──────────────────────────────────────

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_decode/lib_decode.h"
}

#include <array>
#include <map>

/**
 * @brief DRM/KMS display for Xilinx VCU decoded frames — DMA-buf zero-copy import.
 *
 * Wraps DRMDisplayBase with an Allegro-typed API. prepare_fb() imports decoder
 * DMA-buf handles via drmPrimeFDToHandle + drmModeAddFB2, caches per AL_TBuffer*.
 */
class DRMDisplay : public DRMDisplayBase
{
  public:
    /** Typed release callback: matches decoder's return_display_frame signature. */
    using TypedReleaseCallback = std::function<void(AL_TBuffer *)>;

    explicit DRMDisplay(const DRMDisplayConfig &cfg, TypedReleaseCallback release_cb);
    ~DRMDisplay() override;

    void show(AL_TBuffer *frame, const AL_TInfoDecode &info);

  private:
    bool prepare_fb(AL_TBuffer *buf, uint32_t w, uint32_t h, uint32_t &out_fb_id);

  private:
    struct CachedFB
    {
        uint32_t                fb_id = 0;
        std::array<uint32_t, 4> gem_handles{};
        int                     num_gem = 0;
    };

    std::map<AL_TBuffer *, CachedFB> m_fb_cache;
    std::mutex                       m_cache_mutex;
};

// ── DRMDisplayDumb: kernel dumb-buffer (CPU-accessible XRGB8888) ──────────────────

/**
 * @brief DRM display subclass backed by kernel dumb buffers (CPU-accessible XRGB8888).
 *
 * Allocates two dumb framebuffers via DRM_IOCTL_MODE_CREATE_DUMB and registers them
 * with the DRM subsystem.  The caller writes pixel data through pixel_data(), then
 * submits the slot via show().
 *
 * No dependency on the Xilinx DMA allocator or Allegro SDK.
 * Intended for testing DRMDisplayBase without a VCU decoder.
 *
 * @note Pixel format is always DRM_FORMAT_XRGB8888.
 * @throws std::runtime_error on dumb buffer allocation failure.
 */
class DRMDisplayDumb : public DRMDisplayBase
{
  public:
    explicit DRMDisplayDumb(const DRMDisplayConfig &cfg,
                             uint32_t width, uint32_t height,
                             FrameReleaseCallback release_cb);
    ~DRMDisplayDumb() override;

    /** Mutable pointer to the mapped XRGB8888 pixel data of slot i (0 or 1). */
    void *pixel_data(int slot) const;

    /** Display slot i (0 or 1). */
    void show(int slot);

    uint32_t width()  const { return m_width;  }
    uint32_t height() const { return m_height; }

  private:
    struct DumbBuf
    {
        uint32_t gem_handle = 0;
        uint32_t fb_id      = 0;
        uint32_t pitch      = 0;
        uint64_t size       = 0;
        void    *mapped     = nullptr;
    };

    void alloc_dumb_bufs();
    void free_dumb_bufs() noexcept;

    uint32_t m_width;
    uint32_t m_height;
    DumbBuf  m_bufs[2];
};

#endif // DRM_DISPLAY_H
