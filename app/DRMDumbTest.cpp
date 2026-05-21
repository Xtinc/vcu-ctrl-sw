#include "DRMDumbTest.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <chrono>
#include <thread>

// ── DRMDumbTest ───────────────────────────────────────────────────────────────

DRMDumbTest::DRMDumbTest(const Config &cfg) : m_cfg(cfg)
{
    m_display = std::make_unique<DRMDisplayDumb>(cfg.drm, cfg.width, cfg.height, [](void *) {});
}

void DRMDumbTest::run(uint32_t frames)
{
    using namespace std::chrono;
    const nanoseconds frame_interval{1'000'000'000LL / m_cfg.fps};

    auto next       = steady_clock::now();
    auto stat_start = next;

    for (uint32_t i = 0; i < frames; ++i)
    {
        const int slot = static_cast<int>(i & 1);

        // Stamp frame index into the top-left pixel for visual verification.
        if (auto *px = static_cast<uint32_t *>(m_display->pixel_data(slot)))
            px[0] = i;

        m_display->show(slot);

        next += frame_interval;
        std::this_thread::sleep_until(next);

        if ((i + 1) % 100 == 0)
        {
            const auto elapsed_ms =
                duration_cast<milliseconds>(steady_clock::now() - stat_start).count();
            VIDEO_INFO_PRINT("DRMDumbTest: %u frames in %lld ms  (%.1f fps avg)",
                             i + 1, static_cast<long long>(elapsed_ms),
                             (i + 1) * 1000.0 / static_cast<double>(elapsed_ms));
        }
    }

    m_display->stop();
}


#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/mman.h>
#include <thread>

// ── DRMDisplayDumb ────────────────────────────────────────────────────────────

DRMDisplayDumb::DRMDisplayDumb(const DRMDisplayConfig &cfg, uint32_t width, uint32_t height,
                               FrameReleaseCallback release_cb)
    : DRMDisplayBase(cfg, std::move(release_cb)), m_width(width), m_height(height)
{
    alloc_dumb_bufs();
}

DRMDisplayDumb::~DRMDisplayDumb()
{
    // Stop the event thread before freeing DRM objects it may reference.
    stop();
    free_dumb_bufs();
}

void *DRMDisplayDumb::pixel_data(int slot) const
{
    return m_bufs[slot & 1].mapped;
}

void DRMDisplayDumb::show(int slot)
{
    const int i = slot & 1;
    submit(&m_bufs[i], m_bufs[i].fb_id, m_width, m_height);
}

void DRMDisplayDumb::alloc_dumb_bufs()
{
    // Default fill colours: blue (slot 0) / green (slot 1) for easy visual check.
    static const uint32_t kColours[2] = {0x000000FFu, 0x0000FF00u};

    for (int i = 0; i < 2; ++i)
    {
        DumbBuf &b = m_bufs[i];

        // ── Create dumb buffer ─────────────────────────────────────────────
        drm_mode_create_dumb create{};
        create.width = m_width;
        create.height = m_height;
        create.bpp = 32; // XRGB8888
        if (drmIoctl(drm_fd(), DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0)
            throw std::runtime_error(std::string("DRMDisplayDumb: CREATE_DUMB: ") + ::strerror(errno));

        b.gem_handle = create.handle;
        b.pitch = create.pitch;
        b.size = create.size;

        // ── Register as DRM framebuffer ────────────────────────────────────
        uint32_t handles[4] = {create.handle};
        uint32_t pitches[4] = {create.pitch};
        uint32_t offsets[4] = {};
        if (drmModeAddFB2(drm_fd(), m_width, m_height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &b.fb_id, 0) !=
            0)
            throw std::runtime_error(std::string("DRMDisplayDumb: AddFB2: ") + ::strerror(errno));

        // ── Map for CPU access ─────────────────────────────────────────────
        drm_mode_map_dumb map_dumb{};
        map_dumb.handle = create.handle;
        if (drmIoctl(drm_fd(), DRM_IOCTL_MODE_MAP_DUMB, &map_dumb) != 0)
            throw std::runtime_error(std::string("DRMDisplayDumb: MAP_DUMB: ") + ::strerror(errno));

        b.mapped = ::mmap(nullptr, static_cast<size_t>(b.size), PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd(),
                          static_cast<off_t>(map_dumb.offset));
        if (b.mapped == MAP_FAILED)
        {
            b.mapped = nullptr;
            throw std::runtime_error(std::string("DRMDisplayDumb: mmap: ") + ::strerror(errno));
        }

        // ── Initial solid fill ─────────────────────────────────────────────
        auto *px = static_cast<uint32_t *>(b.mapped);
        for (uint64_t p = 0; p < b.size / 4; ++p)
            px[p] = kColours[i];

        VIDEO_DEBUG_PRINT("DRMDisplayDumb: buf[%d] gem=%u fb=%u pitch=%u size=%llu", i, b.gem_handle, b.fb_id, b.pitch,
                          static_cast<unsigned long long>(b.size));
    }
}

void DRMDisplayDumb::free_dumb_bufs() noexcept
{
    for (auto &b : m_bufs)
    {
        if (b.mapped)
        {
            ::munmap(b.mapped, static_cast<size_t>(b.size));
            b.mapped = nullptr;
        }
        if (b.fb_id)
        {
            drmModeRmFB(drm_fd(), b.fb_id);
            b.fb_id = 0;
        }
        if (b.gem_handle)
        {
            drm_mode_destroy_dumb dd{};
            dd.handle = b.gem_handle;
            drmIoctl(drm_fd(), DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
            b.gem_handle = 0;
        }
    }
}

// ── DRMDumbTest ───────────────────────────────────────────────────────────────

DRMDumbTest::DRMDumbTest(const Config &cfg) : m_cfg(cfg)
{
    // Dumb buffers are display-owned; the release callback is a no-op.
    m_display = std::make_unique<DRMDisplayDumb>(cfg.drm, cfg.width, cfg.height, [](void *) {});
}

void DRMDumbTest::run(uint32_t frames)
{
    using namespace std::chrono;
    const nanoseconds frame_interval{1'000'000'000LL / m_cfg.fps};

    auto next = steady_clock::now();
    auto stat_start = next;

    for (uint32_t i = 0; i < frames; ++i)
    {
        const int slot = static_cast<int>(i & 1);

        // Stamp frame index into the top-left pixel for visual verification.
        if (auto *px = static_cast<uint32_t *>(m_display->pixel_data(slot)))
            px[0] = i;

        m_display->show(slot);

        next += frame_interval;
        std::this_thread::sleep_until(next);

        if ((i + 1) % 100 == 0)
        {
            const auto elapsed_ms = duration_cast<milliseconds>(steady_clock::now() - stat_start).count();
            VIDEO_INFO_PRINT("DRMDumbTest: %u frames in %lld ms  (%.1f fps avg)", i + 1,
                             static_cast<long long>(elapsed_ms), (i + 1) * 1000.0 / static_cast<double>(elapsed_ms));
        }
    }

    m_display->stop();
}
