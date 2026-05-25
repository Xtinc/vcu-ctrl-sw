#include "DecMgr.h"

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_rtos/message.h"
}

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

DecMgr::DecMgr(DecMgrConfig cfg) : m_cfg(std::move(cfg))
{
}

DecMgr::~DecMgr()
{
    stop();
}

// ---------------------------------------------------------------------------
// start
// ---------------------------------------------------------------------------

bool DecMgr::start()
{
    if (m_running.exchange(true))
    {
        VIDEO_ERROR_PRINT("DecMgr::start() called while already running");
        return false;
    }

    try
    {
        m_display = std::make_unique<DRMDisplay>(m_cfg.drm, [this](AL_TBuffer *f) {
            return_frame(f);
        });
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

    return true;
}

// ---------------------------------------------------------------------------
// stop
//
// Drain order with lifetime guarantees:
//   1. decoder.flush()    — SDK delivers all remaining frames to on_decoded_frame
//                           → display.show(); m_decoder remains valid.
//   2. display.drain()    — blocks until all frames leave the display pipeline;
//                           all return_frame() calls complete before drain() returns.
//   3. display.reset()    — safe; no more return_frame() calls possible.
//   4. m_decoder.reset()  — safe; all frames already returned to pool.
//
// Key guarantee: m_decoder outlives m_display, so return_frame() never sees null.
//
// NOTE: m_dec_mutex must NOT be held across flush()/drain(). Doing so would
// deadlock because the SDK delivery thread may synchronously call back into
// return_frame() (via display submit → evict → release_frame), and would
// then block on the very mutex held by the flusher.  push_stream() is single-
// producer and stop() never overlaps with it (documented contract), so no
// concurrent mutation of m_decoder can happen here.
// ---------------------------------------------------------------------------

void DecMgr::stop()
{
    if (!m_running.exchange(false))
        return;

    // Step 1: flush decoder — blocks until all frames are delivered to on_decoded_frame.
    //         No lock: callbacks must be free to acquire m_dec_mutex.
    if (m_decoder && !m_decoder->flush())
        VIDEO_ERROR_PRINT("DecMgr: decoder flush timed out; output may be incomplete");

    // Step 2: drain display — blocks until all held frames are released.
    //         After drain() returns, no more return_frame() calls will occur.
    if (m_display)
        m_display->drain();

    // Step 3 + 4: destroy display, then decoder.  Mutex only protects fps() readers.
    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        m_display.reset();
        m_decoder.reset();
    }
}

// ---------------------------------------------------------------------------
// push_stream
//
// Fast path: push directly.  On failure, rebuild the decoder in-place and
// retry once — transparent to the caller.
// ---------------------------------------------------------------------------

bool DecMgr::push_stream(const void *data, size_t size, uint8_t flags)
{
    if (!m_running.load(std::memory_order_acquire))
        return false;

    // Fast path.  Single producer thread; no concurrent mutation of m_decoder.
    if (m_decoder && m_decoder->push_stream(data, size, flags))
        return true;

    // Decoder failed or unavailable — rebuild and retry.
    VIDEO_ERROR_PRINT("DecMgr: decoder error detected, rebuilding");
    if (!do_rebuild())
        return false;

    return m_decoder && m_decoder->push_stream(data, size, flags);
}

// ---------------------------------------------------------------------------
// do_rebuild  (called from push_stream on decoder fault)
//
// Same locking discipline as stop(): never hold m_dec_mutex across flush() or
// drain(), to avoid deadlocking the SDK delivery thread.  push_stream is single-
// producer, so concurrent rebuilds are impossible.
// ---------------------------------------------------------------------------

bool DecMgr::do_rebuild()
{
    // Step 1: flush old decoder (likely already Done; returns immediately).
    if (m_decoder)
        m_decoder->flush();

    // Step 2: drain display — blocks until all DRM slots are FREE.
    //         After drain() returns, all return_frame() callbacks have completed.
    if (m_display)
        m_display->drain();

    // Step 3: destroy old decoder — safe; all frames returned.
    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        m_decoder.reset();
    }

    // Step 4: create fresh decoder.
    if (!create_decoder())
    {
        VIDEO_ERROR_PRINT("DecMgr: decoder rebuild failed — pipeline stopped");
        m_running.store(false);
        return false;
    }

    VIDEO_INFO_PRINT("DecMgr: decoder rebuilt successfully");
    return true;
}

// ---------------------------------------------------------------------------
// fps
// ---------------------------------------------------------------------------

double DecMgr::fps() const
{
    if (!m_running.load(std::memory_order_acquire))
        return 0.0;

    std::lock_guard<std::mutex> lk(m_dec_mutex);
    return m_decoder ? m_decoder->fps() : 0.0;
}

// ---------------------------------------------------------------------------
// on_decoded_frame  (called from SDK internal thread)
//
// Invariant: m_display is alive whenever this callback can fire.  stop() drains
// the decoder before resetting m_display, so once m_display is null no SDK
// callbacks remain in flight.
// ---------------------------------------------------------------------------

void DecMgr::on_decoded_frame(AL_TBuffer *frame, const AL_TInfoDecode &info)
{
    m_display->show(frame, info);
}

// ---------------------------------------------------------------------------
// return_frame  (called from DRM event thread via display release callback)
//
// Lifetime guarantee: this callback is only active while m_display exists.
// m_display is always destroyed before m_decoder (see stop() and do_rebuild()),
// and drain() ensures all pending callbacks complete before m_display is
// destroyed.  No mutex needed: m_decoder is guaranteed valid here.
// ---------------------------------------------------------------------------

void DecMgr::return_frame(AL_TBuffer *frame)
{
    m_decoder->return_display_frame(frame);
}

// ---------------------------------------------------------------------------
// DecMgr::create_decoder
//
// The decoder callback and the display release callback both capture `this`
// (DecMgr*).  Lifetime is managed by explicit destruction order:
//   - m_display is always destroyed before m_decoder.
//   - drain() ensures all display callbacks complete before destruction.
// ---------------------------------------------------------------------------
bool DecMgr::create_decoder()
{
    std::unique_ptr<RTDecoder> new_dec;
    try
    {
        new_dec = std::make_unique<RTDecoder>(m_cfg.dec,
                                              [this](AL_TBuffer *frame, const AL_TInfoDecode &info) {
                                                  on_decoded_frame(frame, info);
                                              });
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
