#include "EncMgr.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <chrono>
#include <stdexcept>
#include <thread>

// ---------------------------------------------------------------------------
// EncMgr
// ---------------------------------------------------------------------------

EncMgr::EncMgr(EncMgrConfig cfg, EncodedFrameCallback output_cb)
    : m_cfg(std::move(cfg)), m_output_cb(std::move(output_cb))
{
    if (m_cfg.v4l2_dev.empty())
        throw std::invalid_argument("EncMgr: v4l2_dev must not be empty");
    if (!m_output_cb)
        throw std::invalid_argument("EncMgr: output_cb must not be null");
}

EncMgr::~EncMgr()
{
    stop();
}

bool EncMgr::start()
{
    if (m_running.exchange(true))
    {
        VIDEO_ERROR_PRINT("EncMgr::start() called while already running");
        return false;
    }

    try
    {
        m_encoder = std::make_unique<RTEncoderV4L2>(m_cfg.enc, m_output_cb);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("EncMgr: failed to create encoder: %s", e.what());
        m_running.store(false);
        return false;
    }

    m_loop_thread = std::thread(&EncMgr::loop_thread_func, this);
    return true;
}

void EncMgr::stop()
{
    m_running.store(false);
    if (m_loop_thread.joinable())
        m_loop_thread.join();
}

bool EncMgr::set_bitrate(uint32_t target, uint32_t max)
{
    if (!m_encoder)
        return false;
    return m_encoder->set_bitrate(target, max);
}

bool EncMgr::set_framerate(uint32_t fps, uint32_t clk)
{
    if (!m_encoder)
        return false;
    return m_encoder->set_framerate(fps, clk);
}

void EncMgr::request_IDR()
{
    if (m_encoder)
        m_encoder->request_IDR();
}

std::pair<double, double> EncMgr::fps() const
{
    if (!m_encoder)
        return {0.0, 0.0};
    return m_encoder->fps();
}

// ---------------------------------------------------------------------------
// Pipeline helpers
// ---------------------------------------------------------------------------

bool EncMgr::build_pipeline(std::shared_ptr<V4L2Source> &out_src, int width, int height)
{
    const auto &c = m_cfg;
    const size_t num_bufs = m_cfg.enc.num_src_bufs;

    std::shared_ptr<V4L2Source> src;
    try
    {
        src = std::make_shared<V4L2Source>(c.v4l2_dev, width, height,
                                           m_encoder->src_fourCC(), num_bufs,
                                           /*multiple_planes=*/true, c.sync_dev);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("EncMgr: V4L2Source construction failed: %s", e.what());
        return false;
    }

    DMAFdArray fds = m_encoder->acquire_dma_buffers(static_cast<unsigned int>(num_bufs));
    if (fds.empty())
    {
        VIDEO_ERROR_PRINT("EncMgr: acquire_dma_buffers returned empty array");
        return false;
    }

    if (!src->import_fds(std::move(fds)))
    {
        VIDEO_ERROR_PRINT("EncMgr: import_fds failed");
        return false;
    }

    if (!src->start())
    {
        VIDEO_ERROR_PRINT("EncMgr: V4L2Source start failed");
        return false;
    }

    out_src = std::move(src);
    return true;
}

// ---------------------------------------------------------------------------
// Inner capture loop — uses V4L2Source::dqueue() with its built-in poll.
//
// Exits when:
//   - m_running becomes false          (normal stop)
//   - src.has_error()                  (device error or disconnect detected
//                                       internally by V4L2Source)
//   - encoder submit fails
// ---------------------------------------------------------------------------
void EncMgr::run_capture_loop(V4L2Source &src)
{
    while (m_running.load(std::memory_order_relaxed))
    {
        if (src.has_error())
        {
            VIDEO_ERROR_PRINT("EncMgr: V4L2Source entered error state, stopping capture loop");
            break;
        }

        AL_TBuffer *buf = src.dqueue(); // blocks up to 1 s internally
        if (!buf)
            continue; // timeout or transient nullptr — re-check flags

        if (!m_encoder->submit_source_buffer(buf))
        {
            VIDEO_ERROR_PRINT("EncMgr: submit_source_buffer failed");
            break;
        }
    }
}

// ---------------------------------------------------------------------------
// Main loop thread
// ---------------------------------------------------------------------------
void EncMgr::loop_thread_func()
{
    int current_w = static_cast<int>(m_cfg.enc.width);
    int current_h = static_cast<int>(m_cfg.enc.height);
    int retry_count = 0;

    while (m_running.load())
    {
        std::shared_ptr<V4L2Source> src;
        if (!build_pipeline(src, current_w, current_h))
        {
            VIDEO_ERROR_PRINT("EncMgr: pipeline build failed (attempt %d)", retry_count + 1);
            goto recover;
        }

        retry_count = 0;

        // Capture shared_ptr by value so the SDK release callback keeps V4L2Source
        // alive until the last in-flight callback completes, even after src is
        // stopped and reset below (prevents use-after-free on the SDK thread).
        m_encoder->set_release_callback([src](AL_TBuffer const *buf) {
            if (!src->queue(buf))
                VIDEO_ERROR_PRINT("EncMgr: requeue to V4L2 failed");
        });

        run_capture_loop(*src);

        // Clear the release callback: drops the shared_ptr stored in m_release_cb.
        // If the SDK thread already copied the lambda, it holds its own shared_ptr
        // keeping V4L2Source alive until that callback returns — no dangling pointer.
        m_encoder->set_release_callback(nullptr);

        src->stop();
        src.reset(); // releases our shared_ptr; V4L2Source destroyed once SDK is done too

        if (!m_running.load())
            break;

        // Probe the device's current format to detect resolution changes.
        // This is done after destroying the old V4L2Source so the fd is free.
        {
            int probed_w = 0, probed_h = 0;
            const int buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            if (V4L2Source::probe_format(m_cfg.v4l2_dev, buf_type, probed_w, probed_h) &&
                probed_w > 0 && probed_h > 0 &&
                (probed_w != current_w || probed_h != current_h))
            {
                VIDEO_INFO_PRINT("EncMgr: resolution change detected %dx%d -> %dx%d",
                                 current_w, current_h, probed_w, probed_h);
                current_w = probed_w;
                current_h = probed_h;
                m_encoder->set_resolution(static_cast<uint32_t>(current_w),
                                          static_cast<uint32_t>(current_h));
                m_encoder->request_IDR();
            }
        }

    recover:
        if (!m_running.load())
            break;

        const int max = m_cfg.max_reconnect_tries;
        if (max >= 0 && ++retry_count > max)
        {
            VIDEO_ERROR_PRINT("EncMgr: max reconnect attempts (%d) reached, stopping", max);
            break;
        }

        VIDEO_INFO_PRINT("EncMgr: waiting %d ms before reconnect (attempt %d/%s)",
                         m_cfg.reconnect_delay_ms, retry_count,
                         max >= 0 ? std::to_string(max).c_str() : "inf");

        // Interruptible sleep: wake up early if stop() is called
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(m_cfg.reconnect_delay_ms);
        while (m_running.load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    m_running.store(false);

    if (!m_encoder->flush())
        VIDEO_ERROR_PRINT("EncMgr: encoder flush timed out; output may be incomplete");

    m_encoder.reset();
    VIDEO_INFO_PRINT("EncMgr: loop thread exited");
}
