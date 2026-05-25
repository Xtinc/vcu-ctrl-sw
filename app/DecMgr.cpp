#include "DecMgr.h"

extern "C"
{
#include "lib_rtos/message.h"
}

// ---------------------------------------------------------------------------
// DecMgr::create_decoder — two-phase construction to get a self-referential
// weak_ptr inside the SDK callback without creating a reference cycle.
//
//  Phase 1: create a shared<weak_ptr<RTDecoder>> context object (cb_ctx).
//           The lambda captures cb_ctx by shared_ptr so it outlives the call.
//  Phase 2: after construction, write the weak_ptr into *cb_ctx so that the
//           lambda can lock it and obtain the originating decoder.
//
// Calling on_decoded_frame with the weak_ptr (DecRef) lets us correctly
// return frames to their own decoder session even during a rebuild.
// ---------------------------------------------------------------------------
bool DecMgr::create_decoder()
{
    auto cb_ctx = std::make_shared<DecRef>(); // initially empty weak_ptr

    std::shared_ptr<RTDecoder> new_dec;
    try
    {
        new_dec = std::make_shared<RTDecoder>(m_cfg.dec,
                                              [this, cb_ctx](AL_TBuffer *frame, const AL_TInfoDecode &info) {
                                                  on_decoded_frame(frame, info, *cb_ctx);
                                              });
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("DecMgr: failed to create decoder: %s", e.what());
        return false;
    }

    *cb_ctx = new_dec; // complete self-reference (weak_ptr, no cycle)

    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        m_decoder = std::move(new_dec);
    }
    return true;
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

    if (!create_decoder())
    {
        m_running.store(false);
        return false;
    }

    m_state.store(State::Running, std::memory_order_release);
    return true;
}

// ---------------------------------------------------------------------------
// stop
//
// Drain order (mirrors main_dec.cpp):
//   1. decoder.flush()  — all frames reach on_decoded_frame / display
//   2. display.stop()   — releases the last held frame back to the decoder
//   3. decoder.reset()  — safe to destroy; all frames already returned
// ---------------------------------------------------------------------------

void DecMgr::stop()
{
    if (!m_running.exchange(false))
        return;

    m_state.store(State::Stopping, std::memory_order_release);

    // Move decoder out of m_decoder so the shared_ptr refcount stays > 0
    // throughout the cleanup sequence — this keeps weak_ptr::lock() working
    // in both the frame callback and the display release callback.
    std::shared_ptr<RTDecoder> dec;
    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        dec = std::move(m_decoder);
    }

    // Step 1: flush decoder — blocks until all frames are delivered to callback.
    if (dec && !dec->flush())
        VIDEO_ERROR_PRINT("DecMgr: decoder flush timed out; output may be incomplete");

    // Step 2: stop display — releases any frame it currently holds via the
    // release callback, which calls dec->return_display_frame() through the
    // weak_ptr (dec is still alive here).
    {
        std::lock_guard<std::mutex> lk(m_disp_mutex);
        if (m_display)
        {
            m_display->stop();
            m_display.reset();
        }
    }

    // Step 3: drop the last strong reference — destructor is a no-op now
    // because flush() has already drained the SDK and the display has returned
    // any held frame.
    dec.reset();
}

// ---------------------------------------------------------------------------
// push_stream
// ---------------------------------------------------------------------------

bool DecMgr::push_stream(const void *data, size_t size, uint8_t flags)
{
    if (!m_running.load(std::memory_order_acquire))
        return false;

    if (m_state.load(std::memory_order_acquire) == State::DecoderFault)
    {
        VIDEO_ERROR_PRINT("DecMgr: push_stream called in DecoderFault state — call rebuild() first");
        return false;
    }

    std::lock_guard<std::mutex> lk(m_dec_mutex);
    if (!m_decoder)
        return false;

    if (!m_decoder->push_stream(data, size, flags))
    {
        VIDEO_ERROR_PRINT("DecMgr: decoder push_stream failed — entering DecoderFault");
        m_state.store(State::DecoderFault, std::memory_order_release);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// flush
// ---------------------------------------------------------------------------

bool DecMgr::flush()
{
    if (!m_running.load(std::memory_order_acquire))
        return true;

    // Obtain an independent strong reference so the decoder stays alive while
    // we call its flush() without holding m_dec_mutex.
    std::shared_ptr<RTDecoder> dec;
    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        dec = m_decoder;
    }

    bool ok = true;
    if (dec)
    {
        ok = dec->flush();
        if (!ok)
            VIDEO_ERROR_PRINT("DecMgr: decoder flush timed out; output may be incomplete");
    }
    dec.reset();

    // Full pipeline teardown — mirrors stop() except we already flushed above,
    // so the second flush() inside stop() will be a no-op (idempotent).
    stop();
    return ok;
}

// ---------------------------------------------------------------------------
// rebuild
//
// Recover from DecoderFault by tearing down the old session and creating
// a fresh decoder.  The display is destroyed here and will be re-opened
// lazily on the first frame from the new decoder.
// ---------------------------------------------------------------------------

bool DecMgr::rebuild()
{
    if (!m_running.load(std::memory_order_acquire))
    {
        VIDEO_ERROR_PRINT("DecMgr::rebuild() called while not running");
        return false;
    }

    VIDEO_INFO_PRINT("DecMgr: rebuilding decoder");

    // Move old decoder out of m_decoder — keep it alive via old_dec so that
    // both the display release callback and any lingering SDK callbacks can
    // still call its return_display_frame().
    std::shared_ptr<RTDecoder> old_dec;
    {
        std::lock_guard<std::mutex> lk(m_dec_mutex);
        old_dec = std::move(m_decoder);
    }

    // Step 1: flush old decoder first (mirrors stop() drain order).
    // In the typical fault case the decoder is already in Done state and this
    // returns immediately.  Any residual SDK callbacks will see m_state ==
    // DecoderFault and will NOT lazily re-create the display (see
    // on_decoded_frame); if m_display still exists those frames are shown
    // normally before we tear down the display in the next step.
    if (old_dec)
        old_dec->flush(); // return value intentionally ignored in error path

    // Step 2: stop display — releases any held frame back to old_dec via the
    // per-session weak_ptr captured in its release callback.  old_dec is still
    // alive here, so orig.lock() is guaranteed to succeed.
    {
        std::lock_guard<std::mutex> lk(m_disp_mutex);
        if (m_display)
        {
            m_display->stop();
            m_display.reset();
        }
    }

    // Step 3: destroy old decoder — all frames have been returned by now.
    old_dec.reset();

    // Create new decoder.
    if (!create_decoder())
    {
        VIDEO_ERROR_PRINT("DecMgr: decoder rebuild failed — stopping pipeline");
        m_running.store(false);
        m_state.store(State::Stopping, std::memory_order_release);
        return false;
    }

    m_state.store(State::Running, std::memory_order_release);
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
// @param orig  Weak reference to the decoder that produced this frame.
//              Locking it keeps the decoder alive long enough to call
//              return_display_frame() if no display is available.
// ---------------------------------------------------------------------------

void DecMgr::on_decoded_frame(AL_TBuffer *frame, const AL_TInfoDecode &info, const DecRef &orig)
{
    std::lock_guard<std::mutex> lk(m_disp_mutex);

    // Lazily open the display on the first decoded frame.
    // Guard with m_state == Running (not m_running) so that display creation
    // is suppressed during rebuild() (m_state == DecoderFault) and after
    // stop() has begun (m_state == Stopping).  Using m_running alone is
    // insufficient because m_running stays true throughout rebuild().
    if (!m_display && m_state.load(std::memory_order_acquire) == State::Running)
    {
        // The display release callback captures a copy of orig (weak_ptr) so
        // that frames are returned to the correct decoder session even if
        // m_decoder has been replaced by a rebuild.
        try
        {
            m_display = std::make_unique<DRMDisplay>(m_cfg.drm, [orig](AL_TBuffer *f) {
                if (auto dec = orig.lock())
                    dec->return_display_frame(f);
            });

            VIDEO_INFO_PRINT("DecMgr: DRMDisplay opened on %s (%dx%d)", m_cfg.drm.drm_device.c_str(),
                             info.tDim.iWidth, info.tDim.iHeight);
        }
        catch (const std::exception &e)
        {
            VIDEO_ERROR_PRINT("DecMgr: DRMDisplay init failed: %s — dropping frame", e.what());
            if (auto dec = orig.lock())
                dec->return_display_frame(frame);
            return;
        }
    }

    if (m_display)
    {
        m_display->show(frame, info);
        return;
    }

    // No display available (pipeline stopping or display open failed):
    // return the frame directly so the decoder pool is not starved.
    if (auto dec = orig.lock())
        dec->return_display_frame(frame);
}
