#include "DecMgr.h"
#include "DRMDisplay.h"
#include "RTDecoder.h"
#include "lib_network/ReliableUDP.h"

static DecoderConfig to_decoder_config(const DecMgrConfig &cfg)
{
    DecoderConfig dec{};
    dec.codec = (cfg.codec == VideoCodec::AVC) ? AL_CODEC_AVC : AL_CODEC_HEVC;
    dec.input_buffer_size = cfg.input_buffer_size;
    dec.low_delay_mode = cfg.low_delay_mode;
    dec.dec_dev_path = cfg.dec_dev;
    dec.input_buffer_num = 4;
    dec.flush_timeout_ms = 5000;
    return dec;
}

static DRMDisplayConfig to_drm_config(const DecMgrConfig &cfg)
{
    DRMDisplayConfig drm{};
    drm.drm_device = cfg.drm_device;
    drm.desired_width = cfg.desired_width;
    drm.desired_height = cfg.desired_height;
    drm.desired_refresh = cfg.desired_refresh;
    drm.connector_id = -1;
    drm.crtc_id = -1;
    drm.plane_id = -1;
    drm.llp2_mode = cfg.low_delay_mode;
    return drm;
}

DecMgr::DecMgr(DecMgrConfig cfg) : m_cfg(std::move(cfg))
{
}

DecMgr::~DecMgr()
{
    stop();
}

bool DecMgr::start()
{
    if (m_running.exchange(true))
    {
        VIDEO_ERROR_PRINT("DecMgr::start() called while already running");
        return false;
    }

    if (m_cfg.udp_local_port == 0)
    {
        VIDEO_ERROR_PRINT("DecMgr: invalid UDP local port 0");
        m_running.store(false);
        return false;
    }

    auto cleanup_start_failure = [this]() {
        if (m_receiver)
        {
            m_receiver->stop();
            m_receiver.reset();
        }
        {
            std::lock_guard<std::mutex> lk(m_dec_mutex);
            m_decoder.reset();
        }
        m_display.reset();
        m_running.store(false);
    };

    try
    {
        m_display = std::make_unique<DRMDisplay>(to_drm_config(m_cfg), [this](AL_TBuffer *f) { return_frame(f); });

        if (!create_decoder())
        {
            throw std::runtime_error("DecMgr: create_decoder failed");
        }

        m_receiver = std::make_shared<ReliableUDP>(BG_SERVICE, m_cfg.udp_local_port);
        m_receiver->set_receive_callback([this](const uint8_t *data, size_t size) {
            if (size < 2)
            {
                return;
            }
            push_stream(data + 1, size - 1, static_cast<StreamFlags>(data[0]));
        });
        m_receiver->start();

        if (!m_cfg.udp_reply_addr.empty() && m_cfg.udp_reply_port > 0)
        {
            if (!m_receiver->add_destination(m_cfg.udp_reply_addr, m_cfg.udp_reply_port))
            {
                VIDEO_ERROR_PRINT("DecMgr: failed to set RTT reply destination %s:%u", m_cfg.udp_reply_addr.c_str(),
                                  m_cfg.udp_reply_port);
            }
        }

        return true;
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("DecMgr: start failed: %s", e.what());
        cleanup_start_failure();
        return false;
    }
}

void DecMgr::stop()
{
    if (!m_running.exchange(false))
        return;

    // Stop receiver — no more push_stream() after this point.
    if (m_receiver)
    {
        m_receiver->stop();
        m_receiver.reset();
    }

    // Flush decoder; no lock — callbacks need m_dec_mutex.
    if (m_decoder && !m_decoder->flush())
        VIDEO_ERROR_PRINT("DecMgr: decoder flush timed out; output may be incomplete");

    // Drain display — after this no more return_frame() calls.
    if (m_display)
        m_display->drain();

    // Destroy display then decoder (display first to maintain lifetime order).
    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        m_display.reset();
        m_decoder.reset();
    }
}

bool DecMgr::push_stream(const void *data, size_t size, StreamFlags flags)
{
    if (!m_running.load(std::memory_order_acquire))
        return false;

    const auto sdk_flags = static_cast<uint8_t>(flags);

    if ((!data && size > 0) || size > m_cfg.input_buffer_size)
    {
        VIDEO_ERROR_PRINT("DecMgr: invalid stream buffer (data=%p, size=%zu, capacity=%u)", data, size,
                          m_cfg.input_buffer_size);
        return false;
    }

    // Fast path.  Single producer thread; no concurrent mutation of m_decoder.
    if (m_decoder && m_decoder->push_stream(data, size, sdk_flags))
        return true;

    // Decoder failed or unavailable — rebuild and retry.
    VIDEO_ERROR_PRINT("DecMgr: decoder error detected, rebuilding");
    if (!do_rebuild())
        return false;

    return m_decoder && m_decoder->push_stream(data, size, sdk_flags);
}

bool DecMgr::do_rebuild()
{
    if (m_decoder)
        m_decoder->flush();

    if (m_display)
        m_display->drain();

    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        m_decoder.reset();
    }

    if (!create_decoder())
    {
        VIDEO_ERROR_PRINT("DecMgr: decoder rebuild failed — pipeline stopped");
        m_display.reset();
        m_running.store(false);
        return false;
    }

    VIDEO_INFO_PRINT("DecMgr: decoder rebuilt successfully");
    return true;
}

double DecMgr::fps() const
{
    if (!m_running.load(std::memory_order_acquire))
        return 0.0;

    std::lock_guard<std::mutex> lk(m_dec_mutex);
    return m_decoder ? m_decoder->fps() : 0.0;
}

double DecMgr::recv_rate() const
{
    if (!m_running.load(std::memory_order_acquire))
        return 0.0;

    auto receiver = m_receiver;
    if (!receiver)
        return 0.0;

    return receiver->recv_rate();
}

double DecMgr::lost_rate() const
{
    if (!m_running.load(std::memory_order_acquire))
        return 0.0;

    auto receiver = m_receiver;
    if (!receiver)
        return 0.0;

    return receiver->lost_rate();
}

std::string DecMgr::queue_stats_text() const
{
    if (!m_running.load(std::memory_order_acquire))
        return std::string{};

    auto receiver = m_receiver;
    if (!receiver)
        return std::string{};

    return receiver->queue_stats_text();
}

int64_t DecMgr::rtt_ms() const
{
    if (!m_running.load(std::memory_order_acquire))
        return -1;

    auto receiver = m_receiver;
    if (!receiver)
        return -1;

    return receiver->rtt_ms();
}

int64_t DecMgr::offset_ms() const
{
    if (!m_running.load(std::memory_order_acquire))
        return 0;

    auto receiver = m_receiver;
    if (!receiver)
        return 0;

    return receiver->offset_ms();
}

void DecMgr::on_decoded_frame(AL_TBuffer *frame, const AL_TInfoDecode &info)
{
    if (m_display)
        m_display->show(frame, info);
}

void DecMgr::return_frame(AL_TBuffer *frame)
{
    if (m_decoder)
        m_decoder->return_display_frame(frame);
}

bool DecMgr::create_decoder()
{
    std::unique_ptr<RTDecoder> new_dec;
    try
    {
        new_dec = std::make_unique<RTDecoder>(
            to_decoder_config(m_cfg),
            [this](AL_TBuffer *frame, const AL_TInfoDecode &info) { on_decoded_frame(frame, info); });
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("DecMgr: failed to create decoder: %s", e.what());
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        m_decoder = std::move(new_dec);
    }
    return true;
}
