#include "DecMgr.h"
#include "BackGround.h"
#include "lib_network/udp_net.h"

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_rtos/message.h"
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

    try
    {
        m_display = std::make_unique<DRMDisplay>(m_cfg.drm, [this](AL_TBuffer *f) { return_frame(f); });
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("DecMgr: failed to create DRMDisplay: %s", e.what());
        m_running.store(false);
        return false;
    }

    if (!create_decoder())
    {
        m_display.reset();
        m_running.store(false);
        return false;
    }

    if (m_cfg.udp_local_port != 0)
    {
        m_receiver = make_reliable_udp(BG_SERVICE, m_cfg.udp_local_port);
        m_receiver->set_receive_callback([this](const uint8_t *data, size_t size) { push_stream(data, size); });
        m_receiver->start();
    }

    return true;
}

void DecMgr::stop()
{
    if (!m_running.exchange(false))
        return;

    // Step 1: stop UDP receiver — joins its worker thread so no more push_stream() calls
    //         can arrive during or after the decoder flush.
    if (m_receiver)
    {
        m_receiver->stop();
        m_receiver.reset();
    }

    // Step 2: flush decoder — blocks until all frames are delivered to on_decoded_frame.
    //         No lock: callbacks must be free to acquire m_dec_mutex.
    if (m_decoder && !m_decoder->flush())
        VIDEO_ERROR_PRINT("DecMgr: decoder flush timed out; output may be incomplete");

    // Step 3: drain display — blocks until all held frames are released.
    //         After drain() returns, no more return_frame() calls will occur.
    if (m_display)
        m_display->drain();

    // Step 4 + 5: destroy display, then decoder.  Mutex only protects fps() readers.
    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        m_display.reset();
        m_decoder.reset();
    }
}

bool DecMgr::push_stream(const void *data, size_t size, uint8_t flags)
{
    if (!m_running.load(std::memory_order_acquire))
        return false;

    if ((!data && size > 0) || size > m_cfg.dec.input_buffer_size)
    {
        VIDEO_ERROR_PRINT("DecMgr: invalid stream buffer (data=%p, size=%zu, capacity=%u)", data, size,
                          m_cfg.dec.input_buffer_size);
        return false;
    }

    // Fast path.  Single producer thread; no concurrent mutation of m_decoder.
    if (m_decoder && m_decoder->push_stream(data, size, flags))
        return true;

    // Decoder failed or unavailable — rebuild and retry.
    VIDEO_ERROR_PRINT("DecMgr: decoder error detected, rebuilding");
    if (!do_rebuild())
        return false;

    return m_decoder && m_decoder->push_stream(data, size, flags);
}

bool DecMgr::do_rebuild()
{
    // Step 1: flush old decoder (likely already Done; returns immediately).
    if (m_decoder)
        m_decoder->flush();

    // Step 2: drain display — blocks until all DRM slots are FREE.
    //         After drain() returns, all return_frame() callbacks have completed
    //         and framebuffer imports for old decoder buffers have been released.
    if (m_display)
        m_display->drain();

    // Step 3: destroy old decoder — safe; all frames returned and display imports released.
    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        m_decoder.reset();
    }

    // Step 4: create fresh decoder.
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

void DecMgr::on_decoded_frame(AL_TBuffer *frame, const AL_TInfoDecode &info)
{
    m_display->show(frame, info);
}

void DecMgr::return_frame(AL_TBuffer *frame)
{
    m_decoder->return_display_frame(frame);
}

bool DecMgr::create_decoder()
{
    std::unique_ptr<RTDecoder> new_dec;
    try
    {
        new_dec = std::make_unique<RTDecoder>(
            m_cfg.dec, [this](AL_TBuffer *frame, const AL_TInfoDecode &info) { on_decoded_frame(frame, info); });
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
