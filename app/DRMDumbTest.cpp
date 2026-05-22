#include "DRMDumbTest.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <chrono>
#include <thread>

DRMDumbTest::DRMDumbTest(const Config &cfg) : m_cfg(cfg)
{
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

int main(int, char **)
{
    DRMDumbTest::Config cfg;
    cfg.drm.drm_device = "/dev/dri/card0";
    cfg.fps = 60;
    DRMDumbTest test(cfg);
    test.run(600); // ~10 seconds
    return 0;
}