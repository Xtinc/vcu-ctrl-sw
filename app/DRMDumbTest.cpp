#include "DRMDumbTest.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <chrono>
#include <thread>

DRMDumbTest::DRMDumbTest(const DRMDisplayConfig &cfg) : m_cfg(cfg)
{
    m_display = std::make_unique<DRMDisplayDumb>(cfg,
                                                static_cast<uint32_t>(cfg.desired_width  ? cfg.desired_width  : 1920),
                                                static_cast<uint32_t>(cfg.desired_height ? cfg.desired_height : 1080),
                                                [](void *) {});
}

void DRMDumbTest::run(uint32_t frames)
{
    using namespace std::chrono;
    const int fps = m_cfg.desired_refresh > 0 ? m_cfg.desired_refresh : 60;
    const nanoseconds frame_interval{1'000'000'000LL / fps};

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
    DRMDisplayConfig cfg;
    cfg.drm_device      = "/dev/dri/card0";
    cfg.desired_width   = 1920;
    cfg.desired_height  = 1080;
    cfg.desired_refresh = 60;
    DRMDumbTest test(cfg);
    test.run(600); // ~10 seconds
    return 0;
}