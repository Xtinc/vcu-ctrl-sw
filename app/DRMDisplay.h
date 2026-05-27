#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

#include "lib_network/ClockWait.h"
#include <xf86drmMode.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

struct DRMDisplayConfig
{
    std::string drm_device = "/dev/dri/card0"; ///< DRM device node path.
    int connector_id = -1;                     ///< KMS connector ID (-1 = auto-detect).
    int crtc_id = -1;                          ///< KMS CRTC ID (-1 = auto-detect).
    int plane_id = -1;                         ///< KMS plane ID (-1 = auto-detect overlay/primary).
    int desired_width = 0;                     ///< Preferred display width in pixels  (0 = no preference).
    int desired_height = 0;                    ///< Preferred display height in pixels (0 = no preference).
    int desired_refresh = 0;                   ///< Preferred refresh rate in Hz       (0 = prefer PREFERRED flag / highest).
    std::chrono::nanoseconds submit_lead_time{4'000'000LL};
};

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

struct CrtcProps
{
    uint32_t active = 0;
    uint32_t mode_id = 0;
};

struct ConnProps
{
    uint32_t crtc_id = 0;
};

/**
 * @brief DRM/KMS double-buffer display base with Atomic API event loop.
 *
 * Manages the event thread, a 4-state double-buffer pipeline, and vblank-timed
 * page-flip submission via ClockEntry.  Subclasses resolve a DRM framebuffer ID
 * from their frame type and call the protected submit().
 *
 * @par Double-buffer state machine
 * @code
 *   FREE ──submit()──> PENDING ──schedule_flip()──> SCANNING
 *    ^   (latest-wins: old PENDING released)             │
 *    │                                                   │ drmModeAtomicCommit
 *    │                                SCANNING ──> RELEASING
 *    └──────────────── on_flip_done: RELEASING ──> FREE + release_cb
 * @endcode
 *
 * @par Thread model
 * - **Producer thread**: submit() — enqueues frames, never blocks.
 * - **Event thread**: all DRM calls — drmModeAtomicCommit, poll, drmHandleEvent.
 *
 * @note Requires @c DRM_CLIENT_CAP_ATOMIC; throws std::runtime_error otherwise.
 * @note Non-copyable, non-movable.
 */
class DRMDisplayBase
{
    enum class SlotState
    {
        FREE,
        PENDING,
        SCANNING,
        RELEASING
    };

    struct Slot
    {
        void *buf = nullptr;
        uint32_t fb_id = 0;
        uint32_t w = 0;
        uint32_t h = 0;
        SlotState state = SlotState::FREE;
    };

  public:
    /**
     * @brief Open the DRM device, enumerate KMS resources, and start the event thread.
     * @param cfg      Display configuration (device path, preferred mode, timing).
     * @throws std::runtime_error     on DRM open / Atomic cap / resource enumeration failure.
     */
    explicit DRMDisplayBase(const DRMDisplayConfig &cfg);
    virtual ~DRMDisplayBase();

    DRMDisplayBase(const DRMDisplayBase &) = delete;
    DRMDisplayBase &operator=(const DRMDisplayBase &) = delete;
    DRMDisplayBase(DRMDisplayBase &&) = delete;
    DRMDisplayBase &operator=(DRMDisplayBase &&) = delete;

    /**
     * @brief Release all held decoder frame references and reset the display pipeline.
     *
     * - Any PENDING frame (not yet committed) is released immediately.
     * - Waits for the in-flight page-flip (if any) to complete via the DRM event.
     * - Releases the SCANNING slot directly without a DRM commit; the kernel's GEM
     *   reference keeps the physical buffer alive until the hardware moves on.
     * - Resets @c m_modeset_done so the next show() performs a full modeset.
     *
     * After drain() returns all decoder buffer references have been returned and the
     * display can be restarted by calling show() again.  Does NOT stop the event
     * thread.  Subclasses may extend drain() to release per-frame GPU resources
     * (e.g. cached DRM framebuffer registrations) after the base has finished.
     */
    virtual void drain();

    /**
     * @brief Drain the event thread and release all held frames via FrameReleaseCallback.
     *
     * Idempotent; safe to call multiple times or before destruction.
     */
    void stop();

  protected:
    /**
     * @brief Enqueue a pre-resolved frame into the display pipeline.
     *
     * Never blocks.  If a PENDING frame already exists it is released immediately
     * (latest-wins policy).  If no free slot is available the new frame is dropped
     * and released via FrameReleaseCallback.
     *
     * @param key    Opaque handle passed back to FrameReleaseCallback when retired.
     * @param fb_id  Registered DRM framebuffer; caller owns its lifetime.
     * @param w      Frame width in pixels.
     * @param h      Frame height in pixels.
     */
    void submit(void *key, uint32_t fb_id, uint32_t w, uint32_t h);

  private:
    static void on_page_flip_cb(int fd, unsigned seq, unsigned tv_sec, unsigned tv_usec, void *user_data);

    void init_drm();
    void close_drm();

    bool do_modeset_locked(uint32_t fb_id, uint32_t w, uint32_t h);
    bool schedule_flip_locked();
    ClockEntry::ClockTP compute_submit_deadline();
    void drain_flip_event();
    void on_flip_done(unsigned tv_sec, unsigned tv_usec);
    void event_thread_fn();

    Slot *slot_by_state_locked(SlotState s);

  protected:
    /**
     * @brief Called whenever a frame handle leaves the pipeline (drop, eviction, flip-done).
     * Subclasses implement typed release logic; base class never stores the callback.
     */
    virtual void release_frame(void *key) noexcept = 0;

    DRMDisplayConfig m_cfg;

    int m_drm_fd{-1};
    uint32_t m_conn_id{0};
    uint32_t m_crtc_id{0};
    uint32_t m_plane_id{0};
    int m_pipe{0};

    uint32_t m_mode_blob_id{0};
    bool m_modeset_done{false};
    PlaneProps m_plane_props{};
    CrtcProps m_crtc_props{};
    ConnProps m_conn_props{};

    Slot m_slots[2];
    bool m_in_flight{false};
    std::mutex m_mutex;
    std::condition_variable m_cv;

    ClockEntry::ClockTP m_last_flip_tp{};
    ClockEntry::Nanos m_frame_ns{16'666'666LL};
    drmModeModeInfo m_selected_mode{};
    int m_in_flight_retries{0};
    ClockEntry::ClockTP m_commit_tp{};
    ClockEntry::Nanos m_wait_phase_adjust{};

    std::atomic<bool> m_stopped{false};
    std::thread m_event_thread;
    ClockEntry m_submit_timer;
};

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_decode/lib_decode.h"
}

/**
 * @brief DRM/KMS display for Xilinx VCU decoded frames — DMA-buf zero-copy import.
 *
 * Wraps DRMDisplayBase with an Allegro-typed API. prepare_fb() imports decoder
 * DMA-buf handles via drmPrimeFDToHandle + drmModeAddFB2, caches per AL_TBuffer*.
 */
class DRMDisplay : public DRMDisplayBase
{
    struct CachedFB
    {
        uint32_t fb_id = 0;
        uint32_t w = 0;
        uint32_t h = 0;
        std::array<uint32_t, 4> gem_handles{};
        int num_gem = 0;
    };

  public:
    /** Typed release callback: matches decoder's return_display_frame signature. */
    using TypedReleaseCallback = std::function<void(AL_TBuffer *)>;

    explicit DRMDisplay(const DRMDisplayConfig &cfg, TypedReleaseCallback release_cb);
    ~DRMDisplay() override;

    void drain() override;
    void show(AL_TBuffer *frame, const AL_TInfoDecode &info);

  private:
    void release_frame(void *key) noexcept override;
    bool prepare_fb(AL_TBuffer *buf, uint32_t w, uint32_t h, uint32_t &out_fb_id);
    void clear_cache() noexcept;
    void destroy_cached_fb(CachedFB &fb) noexcept;

  private:
    TypedReleaseCallback m_typed_cb;
    std::map<AL_TBuffer *, CachedFB> m_fb_cache;
    std::mutex m_cache_mutex;
};

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
    explicit DRMDisplayDumb(const DRMDisplayConfig &cfg, uint32_t width, uint32_t height);
    ~DRMDisplayDumb() override;

    /** Mutable pointer to the mapped XRGB8888 pixel data of slot i (0 or 1). */
    void *pixel_data(int slot) const;

    /** Display slot i (0 or 1). */
    void show(int slot);

    uint32_t width() const
    {
        return m_width;
    }
    uint32_t height() const
    {
        return m_height;
    }

  private:
    void release_frame(void * /*key*/) noexcept override {}

    struct DumbBuf
    {
        uint32_t gem_handle = 0;
        uint32_t fb_id = 0;
        uint32_t pitch = 0;
        uint64_t size = 0;
        void *mapped = nullptr;
    };

    void alloc_dumb_bufs();
    void free_dumb_bufs() noexcept;

    uint32_t m_width;
    uint32_t m_height;
    DumbBuf m_bufs[2];
};

#endif // DRM_DISPLAY_H
