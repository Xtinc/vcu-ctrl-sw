#ifndef DRM_DUMB_TEST_H
#define DRM_DUMB_TEST_H

#include "DRMDisplay.h"

#include <cstdint>
#include <memory>

/**
 * @brief Test driver: exercises DRMDisplayDumb with synthetic solid-colour frames.
 *
 * Fills alternating solid-colour frames into dumb buffers and submits them at
 * the configured fps.  Prints per-100-frame timing statistics to the log.
 *
 * Usage:
 * @code
 *   DRMDumbTest::Config cfg;
 *   cfg.drm.drm_device = "/dev/dri/card0";
 *   cfg.fps = 60;
 *   DRMDumbTest test(cfg);
 *   test.run(600); // ~10 seconds
 * @endcode
 */
class DRMDumbTest
{
  public:
    struct Config
    {
        DRMDisplayConfig drm;
        uint32_t width  = 1920;
        uint32_t height = 1080;
        uint32_t fps    = 60; ///< show() call cadence (frames per second).
    };

    explicit DRMDumbTest(const Config &cfg);

    /** Run for @p frames synthetic frames, then stop. Blocks until complete. */
    void run(uint32_t frames);

  private:
    Config                          m_cfg;
    std::unique_ptr<DRMDisplayDumb> m_display;
};

#endif // DRM_DUMB_TEST_H


/**
 * @brief DRM display subclass backed by kernel dumb buffers (CPU-accessible XRGB8888).
 *
 * Allocates two dumb framebuffers using DRM_IOCTL_MODE_CREATE_DUMB and registers
 * them with the DRM subsystem.  The caller can fill the mapped pixel data through
 * pixel_data(), then submit the slot via show() with the matching key().
 *
 * There is no dependency on the Xilinx DMA allocator or Allegro SDK buffer layout.
 *
 * Intended use: test the DRMDisplayBase event loop, state machine, and Atomic API
 * path without a VCU decoder.
 *
 * @note Pixel format is always DRM_FORMAT_XRGB8888.
 * @throws std::runtime_error on dumb buffer allocation failure.
 */
class DRMDisplayDumb : public DRMDisplayBase
{
  public:
    /**
     * @param cfg     DRM device / connector / CRTC / plane config.
     * @param width   Framebuffer width in pixels.
     * @param height  Framebuffer height in pixels.
     * @param release_cb  Fired when a submitted key is no longer on screen.
     */
    explicit DRMDisplayDumb(const DRMDisplayConfig &cfg,
                             uint32_t width, uint32_t height,
                             FrameReleaseCallback release_cb);
    ~DRMDisplayDumb() override;

    /** Mutable pointer to the mapped XRGB8888 pixel data of slot i (0 or 1). */
    void *pixel_data(int slot) const;

    /** Display slot i. Enqueues the pre-allocated dumb buffer into the pipeline. */
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
        // Address of this struct is used as the opaque key passed to show().
    };

    void alloc_dumb_bufs();
    void free_dumb_bufs() noexcept;

    uint32_t m_width;
    uint32_t m_height;
    DumbBuf  m_bufs[2];
};

// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Standalone test driver: exercises DRMDisplayDumb with synthetic frames.
 *
 * Fills alternating solid-colour frames into dumb buffers and submits them at
 * the configured fps.  Prints per-100-frame timing statistics to the log.
 *
 * Usage:
 * @code
 *   DRMDumbTest::Config cfg;
 *   cfg.drm.drm_device = "/dev/dri/card0";
 *   cfg.fps = 60;
 *   DRMDumbTest test(cfg);
 *   test.run(600); // run for ~10 seconds
 * @endcode
 */
class DRMDumbTest
{
  public:
    struct Config
    {
        DRMDisplayConfig drm;
        uint32_t width  = 1920;
        uint32_t height = 1080;
        uint32_t fps    = 60;  ///< show() call cadence (frames per second).
    };

    explicit DRMDumbTest(const Config &cfg);

    /** Run for @p frames synthetic frames, then stop. Blocks until complete. */
    void run(uint32_t frames);

  private:
    Config                          m_cfg;
    std::unique_ptr<DRMDisplayDumb> m_display;
};

#endif // DRM_DUMB_TEST_H
