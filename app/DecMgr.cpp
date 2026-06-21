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
    dec.input_buffer_num = 6;
    dec.flush_timeout_ms = 50000;
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

bool DecMgr::start(const std::string &udp_reply_addr, uint16_t udp_reply_port)
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

    if (udp_reply_addr.empty() || udp_reply_port == 0)
    {
        VIDEO_ERROR_PRINT("DecMgr: invalid RTT reply destination %s:%u", udp_reply_addr.c_str(), udp_reply_port);
        m_running.store(false);
        return false;
    }

    auto cleanup_start_failure = [this]() {
        if (m_receiver)
        {
            m_receiver->stop();
            m_receiver.reset();
        }
        reset_display(true);
        {
            std::lock_guard<std::mutex> lk(m_dec_mutex);
            m_decoder.reset();
        }
        m_running.store(false);
    };

    try
    {
        {
            std::lock_guard<std::mutex> lk(m_display_mutex);
            m_display = std::make_unique<DRMDisplay>(to_drm_config(m_cfg), [this](AL_TBuffer *f) { return_frame(f); });
            m_display_width = 0;
            m_display_height = 0;
        }

        if (!create_decoder())
        {
            throw std::runtime_error("DecMgr: create_decoder failed");
        }

        m_immediate_mode = false;
        m_sw_next_frame = false;
        m_receiver = std::make_shared<ReliableUDP>(BG_SERVICE, m_cfg.udp_local_port);
        m_receiver->set_receive_callback([this](const std::vector<QueueFrame> &frames, bool allow_immediate) {
            return handle_receive_frames(frames, allow_immediate);
        });

        m_receiver->start();

        if (!m_receiver->add_destination(udp_reply_addr, udp_reply_port))
        {
            VIDEO_ERROR_PRINT("DecMgr: failed to set RTT reply destination %s:%u", udp_reply_addr.c_str(),
                              udp_reply_port);
            throw std::runtime_error("DecMgr: add RTT reply destination failed");
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

    if (m_receiver)
    {
        m_receiver->stop();
        m_receiver.reset();
    }

    if (m_decoder && !m_decoder->flush())
        VIDEO_ERROR_PRINT("DecMgr: decoder flush timed out; output may be incomplete");

    reset_display(true);
    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
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

    if (m_decoder && m_decoder->push_stream(data, size, sdk_flags))
        return true;

    VIDEO_ERROR_PRINT("DecMgr: decoder error detected, rebuilding");
    if (!do_rebuild())
        return false;

    return m_decoder && m_decoder->push_stream(data, size, sdk_flags);
}

bool DecMgr::handle_receive_frames(const std::vector<QueueFrame> &frames, bool allow_immediate)
{
    if (frames.empty())
        return true;

    if (m_immediate_mode)
        return handle_slice_mode(frames.back(), allow_immediate);

    return handle_frame_mode(frames, allow_immediate);
}

bool DecMgr::handle_slice_mode(const QueueFrame &frame, bool allow_immediate)
{
    if (frame.size < sizeof(SLICHead))
        return true;

    const auto *head = reinterpret_cast<const SLICHead *>(frame.data);
    const auto flag = static_cast<StreamFlags>(head->slice_tok);

    if (flag == StreamFlags::Unknown)
        return true;

    if (flag == StreamFlags::EndOfSlice || flag == StreamFlags::EndOfFrame)
        push_stream(frame.data + sizeof(SLICHead), frame.size - sizeof(SLICHead), flag);

    if (!allow_immediate)
        m_sw_next_frame = true;

    if (flag == StreamFlags::EndOfFrame && m_sw_next_frame)
    {
        m_immediate_mode = false;
        m_sw_next_frame = false;
        VIDEO_INFO_PRINT("DecMgr: slice delivery switched to frame-complete mode");
    }

    return true;
}

bool DecMgr::handle_frame_mode(const std::vector<QueueFrame> &frames, bool allow_immediate)
{
    auto latest = std::prev(frames.cend());
    if (latest->size < sizeof(SLICHead))
        return true;

    const auto *tail_head = reinterpret_cast<const SLICHead *>(latest->data);
    const auto tail_flag = static_cast<StreamFlags>(tail_head->slice_tok);

    if (tail_flag == StreamFlags::Unknown)
        return true;

    if (tail_flag == StreamFlags::EndOfSlice)
        return false;

    if (tail_flag != StreamFlags::EndOfFrame)
        return true;

    const uint32_t tgt_idx = tail_head->frame_idx;
    const uint32_t tgt_num = tail_head->slice_num;
    if (tgt_num == 0)
        return true;

    auto begin = latest;
    size_t count = 1;
    while (begin != frames.cbegin())
    {
        auto prev = std::prev(begin);
        const auto *head = reinterpret_cast<const SLICHead *>(prev->data);
        if (head->frame_idx != tgt_idx)
            break;

        begin = prev;
        ++count;
    }

    if (count != tgt_num)
    {
        VIDEO_ERROR_PRINT("DecMgr: incomplete frame %u, got %zu/%u slices", tgt_idx, count, tgt_num);
        return true;
    }

    bool pushed_all = true;
    for (auto it = begin; it != frames.cend(); ++it)
    {
        const auto *head = reinterpret_cast<const SLICHead *>(it->data);
        if (!push_stream(it->data + sizeof(SLICHead), it->size - sizeof(SLICHead),
                         static_cast<StreamFlags>(head->slice_tok)))
        {
            pushed_all = false;
            break;
        }
    }

    if (pushed_all && allow_immediate)
    {
        m_immediate_mode = true;
        m_sw_next_frame = false;
        VIDEO_INFO_PRINT("DecMgr: slice delivery switched to immediate mode");
    }

    return true;
}

bool DecMgr::do_rebuild()
{
    if (m_decoder)
        m_decoder->flush();

    drain_display();

    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        m_decoder.reset();
    }

    if (!create_decoder())
    {
        VIDEO_ERROR_PRINT("DecMgr: decoder rebuild failed — pipeline stopped");
        reset_display(false);
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
    if (!frame)
        return;

    const int info_w = info.tDim.iWidth;
    const int info_h = info.tDim.iHeight;
    if (info_w <= 0 || info_h <= 0)
    {
        VIDEO_ERROR_PRINT("DecMgr: invalid decoded frame dimensions %dx%d", info_w, info_h);
        return_frame(frame);
        return;
    }

    const auto w = static_cast<uint32_t>(info_w);
    const auto h = static_cast<uint32_t>(info_h);

    std::lock_guard<std::mutex> lk(m_display_mutex);
    if (!m_display)
    {
        return_frame(frame);
        return;
    }

    if (m_display_width != w || m_display_height != h)
    {
        if (!ensure_display_resolution_locked(w, h))
        {
            return_frame(frame);
            return;
        }
    }

    m_display->show(frame, info);
}

void DecMgr::return_frame(AL_TBuffer *frame)
{
    std::lock_guard<std::mutex> lk(m_dec_mutex);
    if (m_decoder)
        m_decoder->return_display_frame(frame);
}

void DecMgr::drain_display()
{
    std::lock_guard<std::mutex> lk(m_display_mutex);
    if (m_display)
        m_display->drain();
}

void DecMgr::reset_display(bool drain_first)
{
    std::lock_guard<std::mutex> lk(m_display_mutex);
    if (drain_first && m_display)
        m_display->drain();

    m_display.reset();
    m_display_width = 0;
    m_display_height = 0;
}

bool DecMgr::ensure_display_resolution_locked(uint32_t w, uint32_t h)
{
    if (!m_display)
        return false;

    if (m_display_width == w && m_display_height == h)
        return true;

    if (!m_display->set_output_resolution(w, h))
        return false;

    m_display_width = w;
    m_display_height = h;
    return true;
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
