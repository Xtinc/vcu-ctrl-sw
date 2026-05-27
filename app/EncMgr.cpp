#include "EncMgr.h"
#include "BackGround.h"
#include "lib_network/udp_net.h"

#include <stdexcept>

extern "C"
{
#include "lib_rtos/message.h"
}

const char *EncMgr::state_to_cstr(State s)
{
    switch (s)
    {
    case State::Opening:
        return "Opening";
    case State::Streaming:
        return "Streaming";
    case State::SourceChanged:
        return "SourceChanged";
    case State::EncoderFault:
        return "EncoderFault";
    case State::WaitingSource:
        return "WaitingSource";
    case State::Stopping:
        return "Stopping";
    }
    return "Unknown";
}

EncMgr::EncMgr(EncMgrConfig cfg) : m_cfg(std::move(cfg))
{
    if (m_cfg.v4l2_dev.empty())
        throw std::invalid_argument("EncMgr: v4l2_dev must not be empty");
    if (m_cfg.v4l2_subdev.empty())
        throw std::invalid_argument("EncMgr: v4l2_subdev must not be empty (USB cameras not supported)");
    if (m_cfg.udp_dest_addr.empty())
        throw std::invalid_argument("EncMgr: udp_dest_addr must not be empty");
    if (m_cfg.udp_dest_port == 0)
        throw std::invalid_argument("EncMgr: udp_dest_port must be > 0");
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
        m_sender = std::make_shared<ReliableUDP>(BG_SERVICE, m_cfg.udp_local_port);
        m_sender->start();

        if (!m_sender->add_destination(m_cfg.udp_dest_addr, m_cfg.udp_dest_port))
        {
            VIDEO_ERROR_PRINT("EncMgr: failed to set UDP destination %s:%u", m_cfg.udp_dest_addr.c_str(),
                              m_cfg.udp_dest_port);
            throw std::runtime_error("EncMgr: add_destination failed");
        }

        m_encoder = std::make_unique<RTEncoderV4L2>(m_cfg.enc, make_output_callback());
        m_loop_thread = std::thread(&EncMgr::loop_thread_func, this);
        return true;
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("EncMgr: start failed: %s", e.what());
        m_running.store(false);

        if (m_sender)
        {
            m_sender->stop();
            m_sender.reset();
        }

        m_encoder.reset();
        return false;
    }
}

void EncMgr::stop()
{
    m_running.store(false);
    m_wait_cv.notify_all();

    if (m_loop_thread.joinable())
    {
        m_loop_thread.join();
    }

    if (m_sender)
    {
        m_sender->stop();
        m_sender.reset();
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

double EncMgr::send_rate() const
{
    if (!m_running.load())
        return 0.0;

    auto sender = m_sender;
    if (!sender)
        return 0.0;

    return sender->send_rate();
}

bool EncMgr::open_source(int width, int height)
{
    const size_t num_bufs = m_cfg.enc.num_src_bufs;

    std::shared_ptr<V4L2Source> src;
    try
    {
        src = std::make_shared<V4L2Source>(m_cfg.v4l2_dev, m_cfg.v4l2_subdev, width, height, m_encoder->src_fourCC(),
                                           num_bufs,
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

    std::weak_ptr<V4L2Source> weak_src = m_source;
    m_encoder->set_release_callback([weak_src](AL_TBuffer const *buf) {
        if (auto src = weak_src.lock())
        {
            if (!src->queue(buf))
            {
                VIDEO_ERROR_PRINT("EncMgr: requeue to V4L2 failed");
            }
        }
    });

    return true;
}

void EncMgr::close_source()
{
    if (!m_source)
        return;

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
        m_encoder = std::make_unique<RTEncoderV4L2>(enc_cfg, make_output_callback());
        return true;
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("EncMgr: encoder rebuild failed: %s", e.what());
        return false;
    }
}

bool EncMgr::ensure_encoder_at(int width, int height)
{
    if (m_encoder->set_resolution(static_cast<uint32_t>(width), static_cast<uint32_t>(height)))
    {
        m_encoder->request_IDR();
        return true;
    }
    VIDEO_ERROR_PRINT("EncMgr: set_resolution(%dx%d) failed, rebuilding encoder", width, height);
    return rebuild_encoder(width, height);
}

bool EncMgr::query_source_resolution(int &width, int &height) const
{
    if (m_cfg.v4l2_subdev.empty())
    {
        VIDEO_ERROR_PRINT("EncMgr: v4l2_subdev must be configured (USB cameras not supported)");
        return false;
    }

    if (V4L2Source::probe_subdev_format(m_cfg.v4l2_subdev, 0, width, height))
    {
        VIDEO_INFO_PRINT("EncMgr: source resolution %dx%d", width, height);
        return true;
    }

    VIDEO_INFO_PRINT("EncMgr: no active source on %s", m_cfg.v4l2_subdev.c_str());
    return false;
}

EncodedFrameCallback EncMgr::make_output_callback() const
{
    if (!m_sender)
    {
        return {};
    }

    std::weak_ptr<ReliableUDP> weak_udp = m_sender;
    return [weak_udp](const uint8_t *data, size_t size, bool /*eof*/) {
        if (auto udp = weak_udp.lock())
        {
            udp->send(data, size);
        }
    };
}

bool EncMgr::handle_source_change(int &width, int &height)
{
    auto new_w = width;
    auto new_h = height;
    if (!query_source_resolution(new_w, new_h))
    {
        VIDEO_ERROR_PRINT("EncMgr: query_source_resolution failed after source change");
        return true; // non-fatal — next open_source will retry
    }

    if (new_w == width && new_h == height)
        return true; // spurious event, no actual change

    VIDEO_INFO_PRINT("EncMgr: resolution change %dx%d -> %dx%d", width, height, new_w, new_h);

    if (!ensure_encoder_at(new_w, new_h))
        return false;

    width = new_w;
    height = new_h;
    return true;
}

EncMgr::State EncMgr::on_opening(int &width, int &height)
{
    if (!m_running.load())
    {
        return State::Stopping;
    }

    // Query actual source resolution from sub-device
    auto detected_w = width;
    auto detected_h = height;
    if (!query_source_resolution(detected_w, detected_h))
    {
        VIDEO_INFO_PRINT("EncMgr: no source detected, entering WaitingSource state");
        return State::WaitingSource;
    }

    if (detected_w != width || detected_h != height)
    {
        VIDEO_INFO_PRINT("EncMgr: resolution changed %dx%d -> %dx%d", width, height, detected_w, detected_h);
        if (!ensure_encoder_at(detected_w, detected_h))
        {
            VIDEO_ERROR_PRINT("EncMgr: ensure_encoder_at(%dx%d) failed, stopping", detected_w, detected_h);
            return State::Stopping;
        }
        width = detected_w;
        height = detected_h;
    }

    if (!open_source(width, height))
    {
        VIDEO_ERROR_PRINT("EncMgr: open_source failed, waiting for source");
        return State::WaitingSource;
    }

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
        return State::EncoderFault;
    if (source_changed)
        return State::SourceChanged;
    return State::WaitingSource;
}

EncMgr::State EncMgr::on_source_changed(int &width, int &height)
{
    if (!m_running.load())
        return State::Stopping;

    if (!handle_source_change(width, height))
        return State::Stopping;

    return State::Opening;
}

EncMgr::State EncMgr::on_encoder_fault(int width, int height)
{
    if (!m_running.load())
        return State::Stopping;

    VIDEO_ERROR_PRINT("EncMgr: encoder faulted, rebuilding at %dx%d", width, height);
    if (!rebuild_encoder(width, height))
        return State::Stopping;

    VIDEO_INFO_PRINT("EncMgr: encoder rebuilt successfully");
    return State::Opening;
}

EncMgr::State EncMgr::on_waiting_source()
{
    if (!m_running.load())
        return State::Stopping;

    VIDEO_INFO_PRINT("EncMgr: waiting for source on %s", m_cfg.v4l2_subdev.c_str());

    std::unique_lock<std::mutex> lock(m_wait_mutex);
    m_wait_cv.wait_for(lock, std::chrono::milliseconds(m_cfg.source_check_interval_ms),
                       [this] { return !m_running.load(); });
    lock.unlock();

    if (!m_running.load())
        return State::Stopping;

    int w = 0, h = 0;
    if (query_source_resolution(w, h))
    {
        VIDEO_INFO_PRINT("EncMgr: source detected, returning to Opening");
        return State::Opening;
    }

    return State::WaitingSource;
}

void EncMgr::loop_thread_func()
{
    int width = static_cast<int>(m_cfg.enc.width);
    int height = static_cast<int>(m_cfg.enc.height);
    State state = State::Opening;

    VIDEO_INFO_PRINT("EncMgr: loop thread started, initial state=%s", state_to_cstr(state));

    while (state != State::Stopping)
    {
        State next;
        switch (state)
        {
        case State::Opening:
            next = on_opening(width, height);
            break;
        case State::Streaming:
            next = on_streaming();
            break;
        case State::SourceChanged:
            next = on_source_changed(width, height);
            break;
        case State::EncoderFault:
            next = on_encoder_fault(width, height);
            break;
        case State::WaitingSource:
            next = on_waiting_source();
            break;
        case State::Stopping:
            next = State::Stopping;
            break;
        }

        if (next != state)
        {
            VIDEO_INFO_PRINT("EncMgr: %s -> %s", state_to_cstr(state), state_to_cstr(next));
        }

        state = next;
    }

    m_running.store(false);

    if (m_encoder && !m_encoder->flush())
    {
        VIDEO_ERROR_PRINT("EncMgr: encoder flush timed out; output may be incomplete");
    }

    {
        std::lock_guard<std::mutex> lock(m_enc_mutex);
        m_encoder.reset();
    }
    VIDEO_INFO_PRINT("EncMgr: loop thread exited");
}
