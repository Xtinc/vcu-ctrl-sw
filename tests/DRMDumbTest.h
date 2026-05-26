#ifndef DRM_DUMB_TEST_H
#define DRM_DUMB_TEST_H

#include "DRMDisplay.h"

#include <cstdint>
#include <memory>

/**
 * @brief Test driver: exercises DRMDisplayDumb with synthetic solid-colour frames.
 *
 * Fills alternating solid-colour frames into dumb buffers and submits them at
 * drm.desired_refresh fps.  Prints per-100-frame timing statistics to the log.
 *
 * Usage:
 * @code
 *   DRMDisplayConfig cfg;
 *   cfg.drm_device     = "/dev/dri/card0";
 *   cfg.desired_width  = 1920;
 *   cfg.desired_height = 1080;
 *   cfg.desired_refresh = 60;
 *   DRMDumbTest test(cfg);
 *   test.run(600); // ~10 seconds
 * @endcode
 */
class DRMDumbTest
{
  public:
    explicit DRMDumbTest(const DRMDisplayConfig &cfg);

    /** Run for @p frames synthetic frames, then stop. Blocks until complete. */
    void run(uint32_t frames);

  private:
    DRMDisplayConfig                m_cfg;
    std::unique_ptr<DRMDisplayDumb> m_display;
};

#endif // DRM_DUMB_TEST_H