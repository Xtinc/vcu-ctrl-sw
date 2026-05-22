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