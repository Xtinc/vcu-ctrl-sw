#include "DRMDisplay.h"

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_common/BufferPixMapMeta.h"
#include "lib_common/FourCC.h"
#include "lib_fpga/DmaAllocLinux.h"
#include "lib_rtos/message.h"
}

#include "libdrm/drm_fourcc.h"
#include <drm/drm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

#ifndef DRM_FORMAT_XV15
#define DRM_FORMAT_XV15 fourcc_code('X', 'V', '1', '5')
#endif
#ifndef DRM_FORMAT_XV20
#define DRM_FORMAT_XV20 fourcc_code('X', 'V', '2', '0')
#endif

static int score_drm_mode(const drmModeModeInfo *m, int desired_width, int desired_height, int desired_refresh)
{
    if (desired_width > 0 && m->hdisplay != static_cast<uint16_t>(desired_width))
        return INT_MIN;
    if (desired_height > 0 && m->vdisplay != static_cast<uint16_t>(desired_height))
        return INT_MIN;

    int s = (m->type & DRM_MODE_TYPE_PREFERRED) ? 100 : 0;
    if (desired_refresh > 0)
    {
        int d = static_cast<int>(m->vrefresh) - desired_refresh;
        s += 200 - std::min(d < 0 ? -d : d, 200);
    }
    else
    {
        s += static_cast<int>(m->vrefresh);
    }
    return s;
}

/// Select the best drmModeModeInfo from \p conn based on the caller's preferences.
/// Falls back to the PREFERRED flag, then to the first mode if nothing else matches.
/// Returns nullptr only when \p conn has no modes at all.
static const drmModeModeInfo *select_best_mode(const drmModeConnector *conn, int desired_width, int desired_height,
                                               int desired_refresh)
{
    const drmModeModeInfo *best = nullptr;
    int best_score = INT_MIN;

    for (int i = 0; i < conn->count_modes; ++i)
    {
        int s = score_drm_mode(&conn->modes[i], desired_width, desired_height, desired_refresh);
        if (s > best_score)
        {
            best_score = s;
            best = &conn->modes[i];
        }
    }

    // Fallback when the dimension filter rejected every mode.
    if (!best || best_score == INT_MIN)
    {
        for (int i = 0; i < conn->count_modes; ++i)
            if (conn->modes[i].type & DRM_MODE_TYPE_PREFERRED)
            {
                return &conn->modes[i];
            }
        if (conn->count_modes > 0)
            return &conn->modes[0];
        return nullptr;
    }
    return best;
}

static uint32_t fourcc_to_drm_fourcc(uint32_t al_fourcc)
{
    switch (al_fourcc)
    {
    case FOURCC(NV12):
        return DRM_FORMAT_NV12;
    case FOURCC(XV15):
        return DRM_FORMAT_XV15;
    case FOURCC(NV16):
        return DRM_FORMAT_NV16;
    case FOURCC(XV20):
        return DRM_FORMAT_XV20;
    default:
        return 0;
    }
}

static drmModeConnector *find_first_connected_connector(int fd, drmModeRes *res)
{
    for (int i = 0; i < res->count_connectors; ++i)
    {
        auto *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
        {
            return conn;
        }
        drmModeFreeConnector(conn);
    }
    return nullptr;
}

static drmModeCrtc *find_crtc_for_connector(int fd, drmModeRes *res, drmModeConnector *conn, int *pipe)
{
    for (int e = 0; e < conn->count_encoders; ++e)
    {
        auto *enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (!enc)
        {
            continue;
        }
        uint32_t crtc_id = enc->crtc_id;
        uint32_t possible = enc->possible_crtcs;
        drmModeFreeEncoder(enc);

        if (crtc_id)
        {
            for (int c = 0; c < res->count_crtcs; ++c)
            {
                if (res->crtcs[c] == crtc_id)
                {
                    if (pipe)
                    {
                        *pipe = c;
                    }
                    return drmModeGetCrtc(fd, crtc_id);
                }
            }
        }
        if (possible)
        {
            int idx = __builtin_ctz(possible);
            if (idx < res->count_crtcs)
            {
                if (pipe)
                {
                    *pipe = idx;
                }
                return drmModeGetCrtc(fd, res->crtcs[idx]);
            }
        }
    }
    return nullptr;
}

static drmModePlane *find_plane_for_crtc(int fd, drmModePlaneRes *pres, int pipe, int prefer_type)
{
    if (pipe < 0)
    {
        return nullptr;
    }

    drmModePlane *fallback = nullptr;
    for (uint32_t i = 0; i < pres->count_planes; ++i)
    {
        auto *plane = drmModeGetPlane(fd, pres->planes[i]);
        if (!plane || !(plane->possible_crtcs & (1u << pipe)))
        {
            drmModeFreePlane(plane);
            continue;
        }
        int plane_type = -1;
        auto *props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
        if (props)
        {
            for (uint32_t p = 0; p < props->count_props; ++p)
            {
                auto *prop = drmModeGetProperty(fd, props->props[p]);
                if (prop && std::strcmp(prop->name, "type") == 0)
                {
                    plane_type = static_cast<int>(props->prop_values[p]);
                }
                drmModeFreeProperty(prop);
            }
            drmModeFreeObjectProperties(props);
        }
        if (plane_type == prefer_type)
        {
            drmModeFreePlane(fallback);
            return plane;
        }
        if (!fallback)
        {
            fallback = plane;
        }
        else
        {
            drmModeFreePlane(plane);
        }
    }
    return fallback;
}

static uint32_t get_prop_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
{
    auto *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props)
    {
        return 0;
    }
    uint32_t id = 0;
    for (uint32_t i = 0; i < props->count_props && !id; ++i)
    {
        auto *prop = drmModeGetProperty(fd, props->props[i]);
        if (prop && std::strcmp(prop->name, name) == 0)
        {
            id = prop->prop_id;
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return id;
}

void DRMDisplayBase::on_page_flip_cb(int /*fd*/, unsigned /*seq*/, unsigned tv_sec, unsigned tv_usec, void *user_data)
{
    static_cast<DRMDisplayBase *>(user_data)->on_flip_done(tv_sec, tv_usec);
}

DRMDisplayBase::DRMDisplayBase(const DRMDisplayConfig &cfg, FrameReleaseCallback release_cb)
    : m_cfg(cfg), m_release_cb(std::move(release_cb))
{
    if (!m_release_cb)
    {
        throw std::invalid_argument("DRMDisplay: release_cb must not be null");
    }

    init_drm();
    m_adaptive_lead_time = m_cfg.submit_lead_time;
    m_event_thread = std::thread(&DRMDisplayBase::event_thread_fn, this);

    sched_param sp{};
    sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
    if (::pthread_setschedparam(m_event_thread.native_handle(), SCHED_FIFO, &sp) != 0)
    {
        VIDEO_ERROR_PRINT("DRMDisplay: failed to set real-time priority: %s", ::strerror(errno));
    }
}

DRMDisplayBase::~DRMDisplayBase()
{
    stop();
    if (m_mode_blob_id)
    {
        drmModeDestroyPropertyBlob(m_drm_fd, m_mode_blob_id);
    }
    close_drm();
}

void DRMDisplayBase::stop()
{
    if (m_stopped.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }

    m_submit_timer.cancel();
    m_cv.notify_all();

    if (m_event_thread.joinable())
    {
        m_event_thread.join();
    }

    std::lock_guard<std::mutex> lk(m_mutex);
    for (auto &s : m_slots)
    {
        if (s.buf)
        {
            m_release_cb(s.buf);
            s = Slot{};
        }
    }
}

void DRMDisplayBase::submit(void *key, uint32_t fb_id, uint32_t w, uint32_t h)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto *pending = slot_by_state_locked(SlotState::PENDING);
        if (pending)
        {
            m_release_cb(pending->buf);
            *pending = Slot{};
        }

        auto *target = slot_by_state_locked(SlotState::FREE);
        if (!target)
        {
            VIDEO_ERROR_PRINT("DRMDisplay: no free slot, dropping frame");
            m_release_cb(key);
            return;
        }

        target->buf = key;
        target->fb_id = fb_id;
        target->w = w;
        target->h = h;
        target->state = SlotState::PENDING;
    }

    m_cv.notify_one();
}

void DRMDisplayBase::init_drm()
{
    m_drm_fd = ::open(m_cfg.drm_device.c_str(), O_RDWR | O_CLOEXEC);
    if (m_drm_fd < 0)
    {
        throw std::runtime_error("DRMDisplay: cannot open '" + m_cfg.drm_device + "': " + ::strerror(errno));
    }

    drmSetClientCap(m_drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);
    // DRM_CAP_TIMESTAMP_MONOTONIC is a device cap queried via drmGetCap, not a client cap.
    // On Linux >= 3.x the vblank timestamps are always CLOCK_MONOTONIC; nothing to set.

    if (drmSetClientCap(m_drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) != 0)
    {
        close_drm();
        throw std::runtime_error("DRMDisplay: kernel does not support "
                                 "DRM_CLIENT_CAP_ATOMIC");
    }

    auto *res = drmModeGetResources(m_drm_fd);
    if (!res)
    {
        close_drm();
        throw std::runtime_error("DRMDisplay: drmModeGetResources failed");
    }

    drmModeConnector *conn = (m_cfg.connector_id >= 0)
                                 ? drmModeGetConnector(m_drm_fd, static_cast<uint32_t>(m_cfg.connector_id))
                                 : find_first_connected_connector(m_drm_fd, res);
    if (!conn)
    {
        drmModeFreeResources(res);
        close_drm();
        throw std::runtime_error(m_cfg.connector_id >= 0
                                     ? "DRMDisplay: connector " + std::to_string(m_cfg.connector_id) + " not found"
                                     : "DRMDisplay: no connected connector found");
    }

    m_conn_id = conn->connector_id;

    {
        const auto *best = select_best_mode(conn, m_cfg.desired_width, m_cfg.desired_height, m_cfg.desired_refresh);
        if (best)
        {
            m_selected_mode = *best;
            if (best->htotal && best->vtotal && best->clock)
                m_frame_ns =
                    ClockEntry::Nanos(static_cast<int64_t>(best->htotal) * best->vtotal * 1'000'000LL / best->clock);
            VIDEO_DEBUG_PRINT("DRMDisplay: selected mode %dx%d@%uHz (clock=%ukHz) -> frame %.3f ms", best->hdisplay,
                              best->vdisplay, best->vrefresh, best->clock, m_frame_ns.count() * 1e-6);
        }
        else
        {
            VIDEO_ERROR_PRINT("DRMDisplay: connector has no modes, modeset will fail");
        }
    }

    drmModeCrtc *crtc = nullptr;
    if (m_cfg.crtc_id >= 0)
    {
        crtc = drmModeGetCrtc(m_drm_fd, static_cast<uint32_t>(m_cfg.crtc_id));
        m_pipe = -1;
        for (int i = 0; i < res->count_crtcs; ++i)
            if (res->crtcs[i] == static_cast<uint32_t>(m_cfg.crtc_id))
            {
                m_pipe = i;
                break;
            }
    }
    else
    {
        crtc = find_crtc_for_connector(m_drm_fd, res, conn, &m_pipe);
    }
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    if (!crtc)
    {
        close_drm();
        throw std::runtime_error("DRMDisplay: cannot find CRTC for connector");
    }
    if (m_pipe < 0)
    {
        drmModeFreeCrtc(crtc);
        close_drm();
        throw std::runtime_error("DRMDisplay: selected CRTC is not present in DRM resources");
    }
    m_crtc_id = crtc->crtc_id;
    drmModeFreeCrtc(crtc);

    auto *pres = drmModeGetPlaneResources(m_drm_fd);
    if (!pres)
    {
        close_drm();
        throw std::runtime_error("DRMDisplay: drmModeGetPlaneResources failed");
    }
    drmModePlane *plane = nullptr;
    if (m_cfg.plane_id >= 0)
    {
        plane = drmModeGetPlane(m_drm_fd, static_cast<uint32_t>(m_cfg.plane_id));
    }
    else
    {
        plane = find_plane_for_crtc(m_drm_fd, pres, m_pipe, DRM_PLANE_TYPE_PRIMARY);
        if (!plane)
        {
            plane = find_plane_for_crtc(m_drm_fd, pres, m_pipe, DRM_PLANE_TYPE_OVERLAY);
        }
    }
    drmModeFreePlaneResources(pres);
    if (!plane)
    {
        close_drm();
        throw std::runtime_error("DRMDisplay: cannot find a suitable plane");
    }
    m_plane_id = plane->plane_id;
    drmModeFreePlane(plane);

    auto gp_plane = [&](const char *n) { return get_prop_id(m_drm_fd, m_plane_id, DRM_MODE_OBJECT_PLANE, n); };
    auto gp_crtc = [&](const char *n) { return get_prop_id(m_drm_fd, m_crtc_id, DRM_MODE_OBJECT_CRTC, n); };
    auto gp_conn = [&](const char *n) { return get_prop_id(m_drm_fd, m_conn_id, DRM_MODE_OBJECT_CONNECTOR, n); };

    m_plane_props = {
        gp_plane("FB_ID"),  gp_plane("CRTC_ID"), gp_plane("CRTC_X"), gp_plane("CRTC_Y"), gp_plane("CRTC_W"),
        gp_plane("CRTC_H"), gp_plane("SRC_X"),   gp_plane("SRC_Y"),  gp_plane("SRC_W"),  gp_plane("SRC_H"),
    };
    m_crtc_props = {gp_crtc("ACTIVE"), gp_crtc("MODE_ID")};
    m_conn_props = {gp_conn("CRTC_ID")};

    // Validate mandatory plane props.
    if (!m_plane_props.fb_id || !m_plane_props.crtc_id || !m_plane_props.crtc_w || !m_plane_props.crtc_h ||
        !m_plane_props.src_w || !m_plane_props.src_h)
    {
        close_drm();
        throw std::runtime_error("DRMDisplay: plane is missing required atomic properties");
    }
    if (!m_crtc_props.active || !m_crtc_props.mode_id)
    {
        close_drm();
        throw std::runtime_error("DRMDisplay: CRTC is missing ACTIVE / MODE_ID properties");
    }
    if (!m_conn_props.crtc_id)
    {
        close_drm();
        throw std::runtime_error("DRMDisplay: connector is missing CRTC_ID property");
    }

    VIDEO_DEBUG_PRINT("DRMDisplay: fd=%d connector=%u crtc=%u plane=%u pipe=%d", m_drm_fd, m_conn_id, m_crtc_id,
                      m_plane_id, m_pipe);
}

void DRMDisplayBase::close_drm()
{
    if (m_drm_fd >= 0)
    {
        ::close(m_drm_fd);
        m_drm_fd = -1;
    }
}

bool DRMDisplayBase::do_modeset_locked(uint32_t fb_id, uint32_t w, uint32_t h)
{
    if (!m_selected_mode.hdisplay)
    {
        VIDEO_ERROR_PRINT("DRMDisplay: no display mode selected, cannot perform modeset");
        return false;
    }

    uint32_t blob_id = 0;
    int ret = drmModeCreatePropertyBlob(m_drm_fd, &m_selected_mode, sizeof(m_selected_mode), &blob_id);
    if (ret != 0)
    {
        VIDEO_ERROR_PRINT("DRMDisplay: drmModeCreatePropertyBlob: %s", ::strerror(errno));
        return false;
    }

    if (m_mode_blob_id)
    {
        drmModeDestroyPropertyBlob(m_drm_fd, m_mode_blob_id);
    }
    m_mode_blob_id = blob_id;

    auto *req = drmModeAtomicAlloc();
    if (!req)
    {
        VIDEO_ERROR_PRINT("DRMDisplay: drmModeAtomicAlloc failed (modeset)");
        return false;
    }

    drmModeAtomicAddProperty(req, m_conn_id, m_conn_props.crtc_id, m_crtc_id);
    drmModeAtomicAddProperty(req, m_crtc_id, m_crtc_props.active, 1);
    drmModeAtomicAddProperty(req, m_crtc_id, m_crtc_props.mode_id, blob_id);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.fb_id, fb_id);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_id, m_crtc_id);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_x, 0);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_y, 0);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_w, w);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_h, h);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_x, 0);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_y, 0);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_w, w << 16);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_h, h << 16);

    constexpr uint32_t kModesetFlags =
        DRM_MODE_ATOMIC_ALLOW_MODESET | DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    ret = drmModeAtomicCommit(m_drm_fd, req, kModesetFlags, this);
    drmModeAtomicFree(req);

    if (ret != 0)
    {
        VIDEO_ERROR_PRINT("DRMDisplay: modeset drmModeAtomicCommit: %s (%d)", ::strerror(errno), errno);
        return false;
    }
    return true;
}

bool DRMDisplayBase::schedule_flip_locked()
{
    auto *pending = slot_by_state_locked(SlotState::PENDING);
    if (!pending)
    {
        return false;
    }

    if (!m_modeset_done)
    {
        if (!do_modeset_locked(pending->fb_id, pending->w, pending->h))
        {
            VIDEO_ERROR_PRINT("DRMDisplay: modeset failed, dropping frame");
            m_release_cb(pending->buf);
            *pending = Slot{};
            return false;
        }
        m_modeset_done = true;
        // do_modeset_locked already committed with PAGE_FLIP_EVENT.
        // Promote PENDING → SCANNING; old SCANNING (none) → RELEASING skipped.
        pending->state = SlotState::SCANNING;
        m_in_flight = true;
        return true;
    }

    auto *req = drmModeAtomicAlloc();
    if (!req)
    {
        VIDEO_ERROR_PRINT("DRMDisplay: drmModeAtomicAlloc failed");
        return false;
    }

    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.fb_id, pending->fb_id);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_id, m_crtc_id);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_x, 0);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_y, 0);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_w, pending->w);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_h, pending->h);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_x, 0);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_y, 0);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_w, pending->w << 16);
    drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_h, pending->h << 16);

    constexpr uint32_t kFlipFlags = DRM_MODE_ATOMIC_NONBLOCK | DRM_MODE_PAGE_FLIP_EVENT;
    int ret = drmModeAtomicCommit(m_drm_fd, req, kFlipFlags, this);
    drmModeAtomicFree(req);

    if (ret != 0)
    {
        VIDEO_ERROR_PRINT("DRMDisplay: drmModeAtomicCommit: %s (%d)", ::strerror(errno), errno);
        return false;
    }

    m_commit_tp = std::chrono::steady_clock::now();

    // State transitions: SCANNING -> RELEASING, PENDING -> SCANNING.
    auto *scanning = slot_by_state_locked(SlotState::SCANNING);
    if (scanning)
    {
        scanning->state = SlotState::RELEASING;
    }

    pending->state = SlotState::SCANNING;
    m_in_flight = true;
    return true;
}

ClockEntry::ClockTP DRMDisplayBase::compute_submit_deadline()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!slot_by_state_locked(SlotState::PENDING))
    {
        return {};
    }

    const auto now = std::chrono::steady_clock::now();
    auto deadline =
        (m_last_flip_tp == ClockEntry::ClockTP{}) ? now : m_last_flip_tp + m_frame_ns - m_adaptive_lead_time;
    if (deadline > now + m_frame_ns)
    {
        deadline = now;
    }

    return deadline;
}

void DRMDisplayBase::drain_flip_event()
{
    drmEventContext evctx{};
    evctx.version = DRM_EVENT_CONTEXT_VERSION;
    evctx.page_flip_handler = on_page_flip_cb;

    const int timeout_ms =
        std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(m_frame_ns / 2).count()));
    struct pollfd pfd{m_drm_fd, POLLIN, 0};

    if (::poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLIN))
    {
        drmHandleEvent(m_drm_fd, &evctx); // -> on_flip_done -> clears m_in_flight
        m_in_flight_retries = 0;
        return;
    }

    if (++m_in_flight_retries <= 8)
    {
        return;
    }

    VIDEO_ERROR_PRINT("DRMDisplay: flip event timeout after %d retries, resetting", m_in_flight_retries);
    std::lock_guard<std::mutex> lk(m_mutex);
    m_in_flight = false;
    m_in_flight_retries = 0;
    m_last_flip_tp = ClockEntry::ClockTP{};
    auto *rel = slot_by_state_locked(SlotState::RELEASING);
    if (rel)
    {
        m_release_cb(rel->buf);
        *rel = Slot{};
    }
}

void DRMDisplayBase::on_flip_done(unsigned tv_sec, unsigned tv_usec)
{
    const auto hw_tp = ClockEntry::ClockTP{std::chrono::seconds(tv_sec) + std::chrono::microseconds(tv_usec)};
    m_last_flip_tp = hw_tp;

    if (m_commit_tp != ClockEntry::ClockTP{})
    {
        const auto commit_to_flip = hw_tp - m_commit_tp;
        VIDEO_DEBUG_PRINT("DRMDisplay: commit -> flip %.1f ms, frame %.1f ms, lead_time %.1f ms",
                          commit_to_flip.count() * 1e-6, m_frame_ns.count() * 1e-6,
                          m_adaptive_lead_time.count() * 1e-6);
        if (commit_to_flip > m_adaptive_lead_time + m_frame_ns / 2)
        {
            const auto prev = m_adaptive_lead_time;
            m_adaptive_lead_time = std::min(m_adaptive_lead_time + m_frame_ns / 16, m_frame_ns / 3);
            VIDEO_ERROR_PRINT("DRMDisplay: vblank missed (commit -> flip %.1f ms, frame %.1f ms). "
                              "submit_lead_time %.1f ms -> %.1f ms.",
                              commit_to_flip.count() * 1e-6, m_frame_ns.count() * 1e-6, prev.count() * 1e-6,
                              m_adaptive_lead_time.count() * 1e-6);
        }
        else
        {
            const auto excess = m_adaptive_lead_time - m_cfg.submit_lead_time;
            if (excess.count() > 0)
            {
                m_adaptive_lead_time -= excess / 100;
            }
        }

        m_commit_tp = {};
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        auto *rel = slot_by_state_locked(SlotState::RELEASING);
        if (rel)
        {
            m_release_cb(rel->buf);
            *rel = Slot{};
        }
        m_in_flight = false;
    }

    m_cv.notify_one();
}

void DRMDisplayBase::event_thread_fn()
{
    while (true)
    {
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv.wait(lk, [this] {
                return m_stopped.load(std::memory_order_acquire) || m_in_flight ||
                       slot_by_state_locked(SlotState::PENDING) != nullptr;
            });
        }

        if (m_stopped.load(std::memory_order_acquire))
        {
            break;
        }

        if (m_in_flight)
        {
            drain_flip_event();
            continue; // re-enter loop to re-check m_in_flight / m_stopped
        }

        const auto submit_at = compute_submit_deadline();
        if (submit_at == ClockEntry::ClockTP{})
        {
            continue; // PENDING was evicted while we waited
        }

        m_submit_timer.reset();

        if (m_submit_timer.wait_until(submit_at) == -1)
        {
            continue;
        }

        if (m_stopped.load(std::memory_order_acquire))
        {
            break;
        }

        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!m_in_flight && slot_by_state_locked(SlotState::PENDING))
            {
                schedule_flip_locked();
            }
        }
    }
}

DRMDisplayBase::Slot *DRMDisplayBase::slot_by_state_locked(SlotState s)
{
    if (m_slots[0].state == s)
    {
        return &m_slots[0];
    }
    else if (m_slots[1].state == s)
    {
        return &m_slots[1];
    }
    else
    {
        return nullptr;
    }
}

// ── DRMDisplay (DMA-buf subclass) ─────────────────────────────────────────────

DRMDisplay::DRMDisplay(const DRMDisplayConfig &cfg, TypedReleaseCallback release_cb)
    : DRMDisplayBase(cfg, [release_cb](void *key) { release_cb(static_cast<AL_TBuffer *>(key)); })
{
}

DRMDisplay::~DRMDisplay()
{
    stop();

    std::lock_guard<std::mutex> lk(m_cache_mutex);
    for (auto &kv : m_fb_cache)
    {
        auto &fb = kv.second;
        if (fb.fb_id)
        {
            drmModeRmFB(m_drm_fd, fb.fb_id);
        }
        for (int k = 0; k < fb.num_gem; ++k)
        {
            if (fb.gem_handles[k])
            {
                drm_gem_close ca{};
                ca.handle = fb.gem_handles[k];
                drmIoctl(m_drm_fd, DRM_IOCTL_GEM_CLOSE, &ca);
            }
        }
    }
}

void DRMDisplay::show(AL_TBuffer *frame, const AL_TInfoDecode &info)
{
    if (!frame)
        return;
    if (m_stopped.load(std::memory_order_acquire))
    {
        m_release_cb(frame);
        return;
    }

    const auto w = static_cast<uint32_t>(info.tDim.iWidth);
    const auto h = static_cast<uint32_t>(info.tDim.iHeight);

    uint32_t fb_id = 0;
    if (!prepare_fb(frame, w, h, fb_id))
    {
        m_release_cb(frame);
        return;
    }

    submit(frame, fb_id, w, h);
}

// ── DRMDisplay::prepare_fb (DMA-buf import) ──────────────────────────────────

bool DRMDisplay::prepare_fb(AL_TBuffer *buf, uint32_t w, uint32_t h, uint32_t &out_fb_id)
{
    auto *meta = reinterpret_cast<AL_TPixMapMetaData *>(AL_Buffer_GetMetaData(buf, AL_META_TYPE_PIXMAP));
    if (!meta)
    {
        VIDEO_ERROR_PRINT("DRMDisplay::prepare_fb: no AL_TPixMapMetaData");
        return false;
    }
    const uint32_t drm_fmt = fourcc_to_drm_fourcc(meta->tFourCC);
    if (!drm_fmt)
    {
        VIDEO_ERROR_PRINT("DRMDisplay::prepare_fb: unsupported pixel format 0x%08X", meta->tFourCC);
        return false;
    }

    // Fast path: cache hit.
    {
        std::lock_guard<std::mutex> lk(m_cache_mutex);
        auto it = m_fb_cache.find(buf);
        if (it != m_fb_cache.end())
        {
            out_fb_id = it->second.fb_id;
            return true;
        }
    }

    // Cache miss: import DMA-buf and register framebuffer.

    const int num_planes = (AL_GetChromaMode(meta->tFourCC) == AL_CHROMA_MONO) ? 1 : 2;
    static const AL_EPlaneId kPlaneIds[2] = {AL_PLANE_Y, AL_PLANE_UV};

    uint32_t bo_handles[4] = {};
    uint32_t pitches[4] = {};
    uint32_t offsets[4] = {};
    uint32_t gem_handles[4] = {};
    int imported_chunk[4] = {-1, -1, -1, -1};
    int num_gem = 0;

    auto *linux_alloc = reinterpret_cast<AL_TLinuxDmaAllocator *>(buf->pAllocator);

    for (int pi = 0; pi < num_planes; ++pi)
    {
        const AL_TPlane &pl = meta->tPlanes[kPlaneIds[pi]];
        pitches[pi] = static_cast<uint32_t>(pl.iPitch);
        offsets[pi] = static_cast<uint32_t>(pl.iOffset);

        // Deduplicate chunks (Y and UV may share the same DMA buffer).
        int gi = -1;
        for (int k = 0; k < num_gem; ++k)
            if (imported_chunk[k] == pl.iChunkIdx)
            {
                gi = k;
                break;
            }

        if (gi < 0)
        {
            int dma_fd = AL_LinuxDmaAllocator_GetFd(linux_alloc, buf->hBufs[pl.iChunkIdx]);
            if (dma_fd < 0)
            {
                VIDEO_ERROR_PRINT("DRMDisplay::prepare_fb: cannot get DMA-buf fd (chunk %d)", pl.iChunkIdx);
                goto cleanup;
            }
            uint32_t gem = 0;
            if (drmPrimeFDToHandle(m_drm_fd, dma_fd, &gem) != 0)
            {
                VIDEO_ERROR_PRINT("DRMDisplay::prepare_fb: drmPrimeFDToHandle: %s", ::strerror(errno));
                goto cleanup;
            }
            gi = num_gem;
            gem_handles[num_gem] = gem;
            imported_chunk[num_gem++] = pl.iChunkIdx;
        }
        bo_handles[pi] = gem_handles[gi];
    }

    {
        uint32_t fb_id = 0;
        if (drmModeAddFB2(m_drm_fd, w, h, drm_fmt, bo_handles, pitches, offsets, &fb_id, 0) != 0)
        {
            VIDEO_ERROR_PRINT("DRMDisplay::prepare_fb: drmModeAddFB2: %s (%d)", ::strerror(errno), errno);
            goto cleanup;
        }

        {
            std::lock_guard<std::mutex> lk(m_cache_mutex);
            CachedFB &cached = m_fb_cache[buf];
            cached.fb_id = fb_id;
            cached.num_gem = num_gem;
            for (int k = 0; k < num_gem; ++k)
                cached.gem_handles[k] = gem_handles[k];
        }

        out_fb_id = fb_id;
        return true;
    }

cleanup:
    for (int k = 0; k < num_gem; ++k)
    {
        drm_gem_close ca{};
        ca.handle = gem_handles[k];
        drmIoctl(m_drm_fd, DRM_IOCTL_GEM_CLOSE, &ca);
    }
    return false;
}

static void fill_rgb888(void *vaddr, uint32_t pitch, uint32_t width, uint32_t height, uint8_t r, uint8_t g, uint8_t b)
{
    for (uint32_t y = 0; y < height; ++y)
    {
        uint8_t *row = static_cast<uint8_t *>(vaddr) + y * pitch;
        for (uint32_t x = 0; x < width; ++x)
        {
            row[x * 3 + 0] = b; // B
            row[x * 3 + 1] = g; // G
            row[x * 3 + 2] = r; // R
        }
    }
}

DRMDisplayDumb::DRMDisplayDumb(const DRMDisplayConfig &cfg, uint32_t width, uint32_t height,
                               FrameReleaseCallback release_cb)
    : DRMDisplayBase(cfg, std::move(release_cb)), m_width(width), m_height(height)
{
    alloc_dumb_bufs();
}

DRMDisplayDumb::~DRMDisplayDumb()
{
    stop(); // must precede free_dumb_bufs()
    free_dumb_bufs();
}

void *DRMDisplayDumb::pixel_data(int slot) const
{
    return m_bufs[slot & 1].mapped;
}

void DRMDisplayDumb::show(int slot)
{
    const int i = slot & 1;
    submit(&m_bufs[i], m_bufs[i].fb_id, m_width, m_height);
}

void DRMDisplayDumb::alloc_dumb_bufs()
{
    // Default solid colours for visual verification: blue / green.
    static const uint32_t kColours[2] = {0x000000FFu, 0x0000FF00u};

    for (int i = 0; i < 2; ++i)
    {
        DumbBuf &b = m_bufs[i];

        drm_mode_create_dumb create{};
        create.width = m_width;
        create.height = m_height;
        create.bpp = 24; // RG24
        if (drmIoctl(m_drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &create) != 0)
        {
            throw std::runtime_error(std::string("DRMDisplayDumb: CREATE_DUMB: ") + ::strerror(errno));
        }

        b.gem_handle = create.handle;
        b.pitch = create.pitch;
        b.size = create.size;

        uint32_t handles[4] = {create.handle};
        uint32_t pitches[4] = {create.pitch};
        uint32_t offsets[4] = {};
        if (drmModeAddFB2(m_drm_fd, m_width, m_height, DRM_FORMAT_RGB888, handles, pitches, offsets, &b.fb_id, 0))
        {
            throw std::runtime_error(std::string("DRMDisplayDumb: AddFB2: ") + ::strerror(errno));
        }

        drm_mode_map_dumb map_dumb{};
        map_dumb.handle = create.handle;
        if (drmIoctl(m_drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &map_dumb))
        {
            throw std::runtime_error(std::string("DRMDisplayDumb: MAP_DUMB: ") + ::strerror(errno));
        }

        b.mapped = ::mmap(nullptr, static_cast<size_t>(b.size), PROT_READ | PROT_WRITE, MAP_SHARED, m_drm_fd,
                          static_cast<off_t>(map_dumb.offset));
        if (b.mapped == MAP_FAILED)
        {
            b.mapped = nullptr;
            throw std::runtime_error(std::string("DRMDisplayDumb: mmap: ") + ::strerror(errno));
        }

        fill_rgb888(b.mapped, b.pitch, m_width, m_height,
                    (kColours[i] >> 16) & 0xFF, // R
                    (kColours[i] >> 8) & 0xFF,  // G
                    (kColours[i] >> 0) & 0xFF); // B

        VIDEO_DEBUG_PRINT("DRMDisplayDumb: buf[%d] gem=%u fb=%u pitch=%u size=%llu", i, b.gem_handle, b.fb_id, b.pitch,
                          static_cast<unsigned long long>(b.size));
    }
}

void DRMDisplayDumb::free_dumb_bufs() noexcept
{
    for (auto &b : m_bufs)
    {
        if (b.mapped)
        {
            ::munmap(b.mapped, static_cast<size_t>(b.size));
            b.mapped = nullptr;
        }
        if (b.fb_id)
        {
            drmModeRmFB(m_drm_fd, b.fb_id);
            b.fb_id = 0;
        }
        if (b.gem_handle)
        {
            drm_mode_destroy_dumb dd{};
            dd.handle = b.gem_handle;
            drmIoctl(m_drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dd);
            b.gem_handle = 0;
        }
    }
}
