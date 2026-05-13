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
    std::lock_guard<std::mutex> lock(m_enc_mutex);
    if (!m_encoder)
        return false;

    if (!m_encoder->set_bitrate(target, max))
        return false;

    m_cfg.enc.target_bitrate = target;
    m_cfg.enc.max_bitrate = (max > 0) ? max : target;
    return true;
}

bool EncMgr::set_framerate(uint32_t fps, uint32_t clk)
{
    std::lock_guard<std::mutex> lock(m_enc_mutex);
    if (!m_encoder)
        return false;

    if (!m_encoder->set_framerate(fps, clk))
        return false;

    m_cfg.enc.framerate = static_cast<uint16_t>(fps);
    m_cfg.enc.clk_ratio = static_cast<uint16_t>(clk);
    return true;
}

void EncMgr::request_IDR()
{
    std::lock_guard<std::mutex> lock(m_enc_mutex);
    if (m_encoder)
        m_encoder->request_IDR();
}

std::pair<double, double> EncMgr::fps() const
{
    std::lock_guard<std::mutex> lock(m_enc_mutex);
    if (!m_encoder)
        return {0.0, 0.0};
    return m_encoder->fps();
}

// ---------------------------------------------------------------------------
// Pipeline helpers
// ---------------------------------------------------------------------------

bool EncMgr::rebuild_encoder(int width, int height)
{
    // Step 1: flush the faulted encoder (returns immediately if state != Running).
    m_encoder->flush();

    // Step 2: extract and destroy the old encoder before constructing the new one.
    // Hardware resources (scheduler, device handle) must be released first.
    // Do the destruction outside the lock to avoid blocking external callers during
    // the potentially slow AL_Encoder_Destroy call.
    {
        std::unique_ptr<RTEncoderV4L2> old_enc;
        {
            std::lock_guard<std::mutex> lock(m_enc_mutex);
            old_enc = std::move(m_encoder); // m_encoder is null from here
        }
        // old_enc destructor runs here, outside the lock
    }

    // Step 3: construct the new encoder with the current working resolution.
    std::unique_ptr<RTEncoderV4L2> new_enc;
    try
    {
        EncoderConfig enc_cfg;
        {
            std::lock_guard<std::mutex> lock(m_enc_mutex);
            enc_cfg = m_cfg.enc;
        }
        enc_cfg.width = static_cast<uint16_t>(width);
        enc_cfg.height = static_cast<uint16_t>(height);
        new_enc = std::make_unique<RTEncoderV4L2>(enc_cfg, m_output_cb);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("EncMgr: encoder rebuild failed: %s", e.what());
        return false;
    }

    // Step 4: publish the new encoder.
    {
        std::lock_guard<std::mutex> lock(m_enc_mutex);
        m_encoder = std::move(new_enc);
    }
    return true;
}

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
// Inner capture loop
//
// Returns:
//   Stopped       - m_running became false (normal stop)
//   SourceChanged - V4L2_EVENT_SOURCE_CHANGE (resolution) received
//   DeviceError   - device error or encoder submit failure
// ---------------------------------------------------------------------------
EncMgr::LoopExitReason EncMgr::run_capture_loop(V4L2Source &src)
{
    while (m_running.load(std::memory_order_relaxed))
    {
        const DQResult result = src.dqueue();
        switch (result.s)
        {
        case DQStatus::OK:
            if (!m_encoder->submit_source_buffer(result.p))
            {
                VIDEO_ERROR_PRINT("EncMgr: submit_source_buffer failed");
                return LoopExitReason::DeviceError;
            }
            break;
        case DQStatus::Timeout:
            break;
        case DQStatus::SourceChanged:
            return LoopExitReason::SourceChanged;
        case DQStatus::Error:
            return LoopExitReason::DeviceError;
        }
    }
    return LoopExitReason::Stopped;
}

void EncMgr::shutdown_pipeline(std::shared_ptr<V4L2Source> &src)
{
    if (!m_encoder)
        return;

    m_encoder->set_release_callback(nullptr);
    if (src)
    {
        src->stop();
        src.reset();
    }
}

bool EncMgr::try_apply_resolution_change(int &current_w, int &current_h)
{
    int probed_w = 0;
    int probed_h = 0;
    const int buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    if (!V4L2Source::probe_format(m_cfg.v4l2_dev, buf_type, probed_w, probed_h) ||
        probed_w <= 0 || probed_h <= 0 ||
        (probed_w == current_w && probed_h == current_h))
    {
        return true;
    }

    VIDEO_INFO_PRINT("EncMgr: resolution change %dx%d -> %dx%d", current_w, current_h, probed_w, probed_h);
    if (m_encoder->set_resolution(static_cast<uint32_t>(probed_w), static_cast<uint32_t>(probed_h)))
    {
        current_w = probed_w;
        current_h = probed_h;
        m_encoder->request_IDR();
        return true;
    }

    VIDEO_ERROR_PRINT("EncMgr: set_resolution(%dx%d) failed, rebuilding encoder", probed_w, probed_h);
    return rebuild_encoder(current_w, current_h);
}

bool EncMgr::wait_before_reconnect(int &retry_count)
{
    if (!m_running.load())
        return false;

    const int max = m_cfg.max_reconnect_tries;
    if (max >= 0 && ++retry_count > max)
    {
        VIDEO_ERROR_PRINT("EncMgr: max reconnect attempts (%d) reached, stopping", max);
        return false;
    }

    VIDEO_INFO_PRINT("EncMgr: waiting %d ms before reconnect (attempt %d/%s)",
                     m_cfg.reconnect_delay_ms, retry_count,
                     max >= 0 ? std::to_string(max).c_str() : "inf");

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(m_cfg.reconnect_delay_ms);
    while (m_running.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return m_running.load();
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
        }

        if (src)
        {
            retry_count = 0;

            // Capture shared_ptr by value so the SDK release callback keeps V4L2Source
            // alive until the last in-flight callback completes, even after src is
            // stopped and reset below (prevents use-after-free on the SDK thread).
            m_encoder->set_release_callback([src](AL_TBuffer const *buf) {
                if (!src->queue(buf))
                    VIDEO_ERROR_PRINT("EncMgr: requeue to V4L2 failed");
            });

            const LoopExitReason reason = run_capture_loop(*src);
            shutdown_pipeline(src);

            if (!m_running.load() || reason == LoopExitReason::Stopped)
                break;

            // Detect encoder fault and rebuild before the next capture session.
            if (!m_encoder->is_running())
            {
                VIDEO_ERROR_PRINT("EncMgr: encoder faulted, rebuilding at %dx%d", current_w, current_h);
                if (!rebuild_encoder(current_w, current_h))
                    break;
                VIDEO_INFO_PRINT("EncMgr: encoder rebuilt successfully");
            }

            if (reason == LoopExitReason::SourceChanged)
            {
                if (!try_apply_resolution_change(current_w, current_h))
                    break;
                retry_count = 0;
                continue; // source change: reconnect immediately without delay
            }
        }

        if (!wait_before_reconnect(retry_count))
            break;
    }

    m_running.store(false);

    // flush() may block up to 5 s; call outside the lock so external callers
    // are not stalled. m_encoder is still exclusively owned by the loop thread
    // here (run_capture_loop has already returned).
    // Guard against null m_encoder (fatal rebuild_encoder failure).
    if (m_encoder)
    {
        if (!m_encoder->flush())
            VIDEO_ERROR_PRINT("EncMgr: encoder flush timed out; output may be incomplete");
    }

    {
        std::lock_guard<std::mutex> lock(m_enc_mutex);
        m_encoder.reset();
    }
    VIDEO_INFO_PRINT("EncMgr: loop thread exited");
}
