#include "EncMgr.h"

extern "C"
{
#include "lib_rtos/message.h"
}

namespace
{
RTEncoderBase::EncodedFrameCallback make_noop_output_callback()
{
    return [](const uint8_t *, size_t) {
        // TODO: forward encoded frames to the future network sender.
    };
}
} // namespace

// ---------------------------------------------------------------------------
// EncMgr — public interface
// ---------------------------------------------------------------------------

EncMgr::EncMgr(EncMgrConfig cfg) : m_cfg(std::move(cfg))
{
    if (m_cfg.v4l2_dev.empty())
        throw std::invalid_argument("EncMgr: v4l2_dev must not be empty");
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
        m_encoder = std::make_unique<RTEncoderV4L2>(m_cfg.enc, make_noop_output_callback());
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
    {
        m_loop_thread.join();
    }
}

bool EncMgr::set_bitrate(uint32_t target, uint32_t max)
{
    if (!m_running.load())
        return false;

    std::lock_guard<std::mutex> lock(m_enc_mutex);
    if (!m_running.load() || !m_encoder)
    {
        return false;
    }
    if (!m_encoder->set_bitrate(target, max))
    {
        return false;
    }
    m_cfg.enc.target_bitrate = target;
    m_cfg.enc.max_bitrate = (max > 0) ? max : target;
    return true;
}

bool EncMgr::set_framerate(uint32_t fps, uint32_t clk)
{
    if (!m_running.load())
        return false;

    std::lock_guard<std::mutex> lock(m_enc_mutex);
    if (!m_running.load() || !m_encoder)
    {
        return false;
    }
    if (!m_encoder->set_framerate(fps, clk))
    {
        return false;
    }
    m_cfg.enc.framerate = static_cast<uint16_t>(fps);
    m_cfg.enc.clk_ratio = static_cast<uint16_t>(clk);
    return true;
}

void EncMgr::request_IDR()
{
    if (!m_running.load())
        return;

    std::lock_guard<std::mutex> lock(m_enc_mutex);
    if (m_running.load() && m_encoder)
        m_encoder->request_IDR();
}

std::pair<double, double> EncMgr::fps() const
{
    if (!m_running.load())
        return {0.0, 0.0};

    std::lock_guard<std::mutex> lock(m_enc_mutex);
    if (!m_running.load() || !m_encoder)
        return {0.0, 0.0};
    return m_encoder->fps();
}

// ---------------------------------------------------------------------------
// Pipeline helpers (called only from the loop thread)
// ---------------------------------------------------------------------------

bool EncMgr::open_source(int width, int height)
{
    const size_t num_bufs = m_cfg.enc.num_src_bufs;

    std::shared_ptr<V4L2Source> src;
    try
    {
        src = std::make_shared<V4L2Source>(m_cfg.v4l2_dev, width, height, m_encoder->src_fourCC(), num_bufs,
                                           /*multiple_planes=*/true, m_cfg.sync_dev);
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

    m_source = std::move(src);

    // Use a weak_ptr in the callback so the lambda never extends the lifetime of
    // m_source. If close_source() has already reset m_source, lock() returns null
    // and the requeue is silently skipped — no use-after-free is possible.
    std::weak_ptr<V4L2Source> weak_src = m_source;
    m_encoder->set_release_callback([weak_src](AL_TBuffer const *buf) {
        if (auto src = weak_src.lock())
            if (!src->queue(buf))
                VIDEO_ERROR_PRINT("EncMgr: requeue to V4L2 failed");
    });

    return true;
}

void EncMgr::close_source()
{
    if (!m_source)
        return;

    // Nullify the callback first so that any residual SDK callbacks after this
    // point are no-ops (the weak_ptr in the old lambda will also expire once
    // m_source is reset below, providing an additional safety net).
    // m_encoder is always non-null here: close_source() is only called from
    // on_streaming(), which requires a live encoder to have been reached.
    m_encoder->set_release_callback(nullptr);

    m_source->stop();
    m_source.reset();
}

bool EncMgr::rebuild_encoder(int width, int height)
{
    std::lock_guard<std::mutex> lock(m_enc_mutex);
    EncoderConfig enc_cfg = m_cfg.enc;
    enc_cfg.width = static_cast<uint16_t>(width);
    enc_cfg.height = static_cast<uint16_t>(height);
    m_encoder.reset(); // release hardware resource before constructing new instance
    try
    {
        m_encoder = std::make_unique<RTEncoderV4L2>(enc_cfg, make_noop_output_callback());
        return true;
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("EncMgr: encoder rebuild failed: %s", e.what());
        return false;
    }
}

bool EncMgr::handle_source_change(int &width, int &height)
{
    int new_w = 0, new_h = 0;
    if (!V4L2Source::probe_format(m_cfg.v4l2_dev, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, new_w, new_h) || new_w <= 0 ||
        new_h <= 0)
    {
        VIDEO_ERROR_PRINT("EncMgr: probe_format failed after source change");
        return true; // non-fatal — next open_source will retry
    }

    if (new_w == width && new_h == height)
        return true; // spurious event, no actual change

    VIDEO_INFO_PRINT("EncMgr: resolution change %dx%d -> %dx%d", width, height, new_w, new_h);

    if (m_encoder->set_resolution(static_cast<uint32_t>(new_w), static_cast<uint32_t>(new_h)))
    {
        m_encoder->request_IDR();
    }
    else
    {
        VIDEO_ERROR_PRINT("EncMgr: set_resolution(%dx%d) failed, rebuilding encoder", new_w, new_h);
        if (!rebuild_encoder(new_w, new_h))
        {
            return false;
        }
    }

    width = new_w;
    height = new_h;
    return true;
}

bool EncMgr::wait_reconnect(int &retry_count)
{
    if (!m_running.load())
        return false;

    const int max = m_cfg.max_reconnect_tries;
    if (max >= 0 && ++retry_count > max)
    {
        VIDEO_ERROR_PRINT("EncMgr: max reconnect attempts (%d) reached, stopping", max);
        return false;
    }

    VIDEO_INFO_PRINT("EncMgr: waiting %d ms before reconnect (attempt %d/%s)", m_cfg.reconnect_delay_ms, retry_count,
                     max >= 0 ? std::to_string(max).c_str() : "inf");

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(m_cfg.reconnect_delay_ms);
    while (m_running.load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    return m_running.load();
}

EncMgr::State EncMgr::on_opening(int width, int height, int &retry_count)
{
    if (!m_running.load())
        return State::Stopping;

    if (!open_source(width, height))
    {
        VIDEO_ERROR_PRINT("EncMgr: open_source failed (attempt %d)", retry_count + 1);
        return State::Reconnecting;
    }

    retry_count = 0;
    return State::Streaming;
}

EncMgr::State EncMgr::on_streaming()
{
    bool source_changed = false;

    while (m_running.load())
    {
        const DQResult result = m_source->dqueue();
        switch (result.s)
        {
        case DQStatus::OK:
            if (!m_encoder->submit_source_buffer(result.p))
            {
                VIDEO_ERROR_PRINT("EncMgr: submit_source_buffer failed");
                goto done;
            }
            break;
        case DQStatus::Timeout:
            break;
        case DQStatus::SourceChanged:
            source_changed = true;
            goto done;
        case DQStatus::Error:
            VIDEO_ERROR_PRINT("EncMgr: V4L2 device error");
            goto done;
        }
    }
done:
    close_source();

    if (!m_running.load())
        return State::Stopping;
    if (!m_encoder->is_running())
        return State::EncoderFault; // fatal SDK error during session
    if (source_changed)
        return State::SourceChanged;
    return State::Reconnecting; // device error or submit failure; encoder still healthy
}

EncMgr::State EncMgr::on_source_changed(int &width, int &height, int &retry_count)
{
    if (!m_running.load())
        return State::Stopping;

    if (!handle_source_change(width, height))
        return State::Stopping;

    retry_count = 0;
    return State::Opening; // reconnect immediately, no delay
}

EncMgr::State EncMgr::on_encoder_fault(int width, int height)
{
    if (!m_running.load())
        return State::Stopping;

    VIDEO_ERROR_PRINT("EncMgr: encoder faulted, rebuilding at %dx%d", width, height);
    if (!rebuild_encoder(width, height))
        return State::Stopping;

    VIDEO_INFO_PRINT("EncMgr: encoder rebuilt successfully");
    return State::Opening; // reconnect immediately, no delay
}

EncMgr::State EncMgr::on_reconnecting(int &retry_count)
{
    return wait_reconnect(retry_count) ? State::Opening : State::Stopping;
}

void EncMgr::loop_thread_func()
{
    int width = static_cast<int>(m_cfg.enc.width);
    int height = static_cast<int>(m_cfg.enc.height);
    int retry_count = 0;
    State state = State::Opening;

    while (state != State::Stopping)
    {
        switch (state)
        {
        case State::Opening:
            state = on_opening(width, height, retry_count);
            break;
        case State::Streaming:
            state = on_streaming();
            break;
        case State::SourceChanged:
            state = on_source_changed(width, height, retry_count);
            break;
        case State::EncoderFault:
            state = on_encoder_fault(width, height);
            break;
        case State::Reconnecting:
            state = on_reconnecting(retry_count);
            break;
        case State::Stopping:
            break;
        }
    }

    m_running.store(false);

    // Final EOS flush. m_source is already null (close_source was called in on_streaming,
    // or open_source never succeeded). flush() may block up to 5 s; call outside the
    // lock so external callers are not stalled.
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
