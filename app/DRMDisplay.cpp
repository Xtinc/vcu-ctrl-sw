/**
 * @file DRMDisplay.cpp
 * @brief DRM/KMS zero-copy display for Xilinx ZYNQ VCU decoded frames.
 *
 * Implements DRMDisplay: a single-purpose PRIME DMA-buf import display pipeline
 * for use with RTDecoder.
 *
 * Core display loop (display thread):
 *  1. create_fb()       check cache; import DMA-buf on first use per buffer
 *  2. drmModeSetCrtc    first frame only (legacy mode-set)
 *  3. drmModeAtomicCommit(NONBLOCK | PAGE_FLIP_EVENT)  [atomic path]
 *     or drmModeSetPlane                               [legacy fallback]
 *  4. Wait for flip-complete event (select + drmHandleEvent)
 *  5. Release previous frame (drmModeRmFB suppressed — cache managed) + FrameReleaseCallback
 */

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
#include <stdexcept>
#include <string>
#include <sys/select.h>
#include <unistd.h>

#ifndef DRM_FORMAT_XV15
#define DRM_FORMAT_XV15 fourcc_code('X', 'V', '1', '5')
#endif
#ifndef DRM_FORMAT_XV20
#define DRM_FORMAT_XV20 fourcc_code('X', 'V', '2', '0')
#endif

static uint32_t allegro_to_drm_fourcc(uint32_t al_fourcc)
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
    case FOURCC(NV24):
        return DRM_FORMAT_NV24;
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
            return conn;
        drmModeFreeConnector(conn); // null-safe
    }
    return nullptr;
}

static drmModeCrtc *find_crtc_for_connector(int fd, drmModeRes *res, drmModeConnector *conn, int *pipe)
{
    for (int e = 0; e < conn->count_encoders; ++e)
    {
        auto *enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (!enc)
            continue;
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
                        *pipe = c;
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
                    *pipe = idx;
                return drmModeGetCrtc(fd, res->crtcs[idx]);
            }
        }
    }
    return nullptr;
}

static drmModePlane *find_plane_for_crtc(int fd, drmModePlaneRes *pres, int pipe, int prefer_type)
{
    drmModePlane *fallback = nullptr;
    for (uint32_t i = 0; i < pres->count_planes; ++i)
    {
        auto *plane = drmModeGetPlane(fd, pres->planes[i]);
        if (!plane || !(plane->possible_crtcs & (1u << pipe)))
        {
            drmModeFreePlane(plane); // null-safe
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
                    plane_type = static_cast<int>(props->prop_values[p]);
                drmModeFreeProperty(prop); // null-safe
            }
            drmModeFreeObjectProperties(props);
        }
        if (plane_type == prefer_type)
        {
            drmModeFreePlane(fallback); // null-safe
            return plane;
        }
        if (!fallback)
            fallback = plane;
        else
            drmModeFreePlane(plane);
    }
    return fallback;
}

static uint32_t get_property_id(int fd, uint32_t obj_id, uint32_t obj_type, const char *name)
{
    auto *props = drmModeObjectGetProperties(fd, obj_id, obj_type);
    if (!props)
        return 0;
    uint32_t id = 0;
    for (uint32_t i = 0; i < props->count_props && !id; ++i)
    {
        auto *prop = drmModeGetProperty(fd, props->props[i]);
        if (prop && std::strcmp(prop->name, name) == 0)
            id = prop->prop_id;
        drmModeFreeProperty(prop); // null-safe
    }
    drmModeFreeObjectProperties(props);
    return id;
}

static void on_page_flip(int, unsigned int, unsigned int, unsigned int, void *user_data)
{
    *static_cast<bool *>(user_data) = true;
}

DRMDisplay::DRMDisplay(const DRMDisplayConfig &cfg, FrameReleaseCallback release_cb)
    : m_cfg(cfg), m_release_cb(std::move(release_cb))
{
    if (!m_release_cb)
        throw std::invalid_argument("DRMDisplay: release_cb must not be null");
    init_drm();
    m_thread = std::thread(&DRMDisplay::display_thread_fn, this);
}

DRMDisplay::~DRMDisplay()
{
    // Stop display thread and release held frame first.
    // do_stop() does NOT close the DRM fd — we do that after cache cleanup.
    do_stop(false);

    // Clean up cached framebuffers while m_drm_fd is still open.
    {
        std::lock_guard<std::mutex> lk(m_cache_mutex);
        for (auto &[buf, cached] : m_fb_cache)
        {
            if (cached.fb_id)
                drmModeRmFB(m_drm_fd, cached.fb_id);
            for (int k = 0; k < cached.num_gem; ++k)
            {
                if (cached.gem_handles[k])
                {
                    drm_gem_close ca{};
                    ca.handle = cached.gem_handles[k];
                    drmIoctl(m_drm_fd, DRM_IOCTL_GEM_CLOSE, &ca);
                }
            }
        }
    }

    close_drm();
}

void DRMDisplay::stop()
{
    do_stop(true);
}

void DRMDisplay::do_stop(bool call_release)
{
    if (m_stopped.exchange(true))
        return;

    m_cv_display.notify_all();
    m_cv_producer.notify_all();

    if (m_thread.joinable())
        m_thread.join();

    // Drain pending queue
    while (!m_pending_queue.empty())
    {
        if (call_release && m_release_cb)
            m_release_cb(m_pending_queue.front().buf);
        m_pending_queue.pop();
    }

    free_frame(m_held, call_release);
    // Note: close_drm() is NOT called here.
    // The destructor calls it after cache cleanup to ensure m_drm_fd remains
    // valid for drmModeRmFB / DRM_IOCTL_GEM_CLOSE during cache teardown.
}

void DRMDisplay::submit_frame(PendingFrame pf)
{
    if (m_stopped.load())
    {
        if (m_release_cb)
            m_release_cb(pf.buf);
        return;
    }

    std::unique_lock<std::mutex> lk(m_mutex);
    m_cv_producer.wait(lk, [this] { return m_pending_queue.size() < MAX_PENDING_FRAMES || m_stopped.load(); });
    if (m_stopped.load())
    {
        lk.unlock();
        if (m_release_cb)
            m_release_cb(pf.buf);
        return;
    }

    m_pending_queue.push(std::move(pf));
    lk.unlock();
    m_cv_display.notify_one();
}

void DRMDisplay::display_thread_fn()
{
    while (true)
    {
        PendingFrame pf{};
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            m_cv_display.wait(lk, [this] { return !m_pending_queue.empty() || m_stopped.load(); });
            if (m_pending_queue.empty())
                break;

            pf = std::move(m_pending_queue.front());
            m_pending_queue.pop();
        }
        m_cv_producer.notify_one();

        uint32_t fb_id = 0;
        if (!create_fb(pf.buf, pf.w, pf.h, fb_id))
        {
            VIDEO_ERROR_PRINT("DRMDisplay: create_fb failed, dropping frame");
            if (m_release_cb)
                m_release_cb(pf.buf);
            continue;
        }

        if (m_first_frame)
        {
            auto *crtc = drmModeGetCrtc(m_drm_fd, m_crtc_id);
            if (crtc && crtc->mode_valid)
            {
                if (drmModeSetCrtc(m_drm_fd, m_crtc_id, fb_id, 0, 0, &m_conn_id, 1, &crtc->mode) != 0)
                    VIDEO_DEBUG_PRINT("DRMDisplay: drmModeSetCrtc: %s (non-fatal)", ::strerror(errno));
            }
            drmModeFreeCrtc(crtc); // null-safe
            m_first_frame = false;
        }

        bool flip_ok = false;
        if (m_atomic)
        {
            auto *req = drmModeAtomicAlloc();
            if (req)
            {
                drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.fb_id, fb_id);
                drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_id, m_crtc_id);
                drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_x, 0);
                drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_y, 0);
                drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_w, pf.w);
                drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.crtc_h, pf.h);
                drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_x, 0);
                drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_y, 0);
                drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_w, pf.w << 16);
                drmModeAtomicAddProperty(req, m_plane_id, m_plane_props.src_h, pf.h << 16);
                bool flip_done = false;
                // vsync on : NONBLOCK + PAGE_FLIP_EVENT — wait for vblank before releasing prev frame.
                // vsync off: NONBLOCK only             — submit immediately, release prev frame at once.
                uint32_t flags = DRM_MODE_ATOMIC_NONBLOCK;
                if (m_cfg.enable_vsync)
                    flags |= DRM_MODE_PAGE_FLIP_EVENT;
                int ret = drmModeAtomicCommit(m_drm_fd, req, flags, &flip_done);
                drmModeAtomicFree(req);
                if (ret == 0)
                {
                    flip_ok = true;
                    if (m_cfg.enable_vsync)
                        wait_flip_event(flip_done);
                }
                else
                {
                    VIDEO_ERROR_PRINT("DRMDisplay: drmModeAtomicCommit: %s (%d)", ::strerror(errno), errno);
                }
            }
        }

        if (!flip_ok)
        {
            // Legacy fallback (kmssink pattern): drmModeSetPlane submits the frame,
            // then drmWaitVBlank ensures the plane update has committed before we
            // release the previous framebuffer.
            if (drmModeSetPlane(m_drm_fd, m_plane_id, m_crtc_id, fb_id, 0, 0, 0, pf.w, pf.h, 0, 0, pf.w << 16,
                                pf.h << 16) != 0)
                VIDEO_ERROR_PRINT("DRMDisplay: drmModeSetPlane: %s (%d)", ::strerror(errno), errno);
            else if (m_cfg.enable_vsync)
                wait_vblank();
        }

        free_frame(m_held, true);
        m_held = {pf.buf, fb_id};
    }
}

void DRMDisplay::wait_vblank()
{
    // Matches kmssink's gst_kms_sink_sync() for the drmModeSetPlane path:
    // wait for the next vblank so the plane update has taken effect and
    // the previous framebuffer is no longer being scanned out.
    drmVBlank vbl{};
    vbl.request.type = DRM_VBLANK_RELATIVE;
    vbl.request.sequence = 1;
    if (m_pipe == 1)
        vbl.request.type |= DRM_VBLANK_SECONDARY;
    else if (m_pipe > 1)
        vbl.request.type |= static_cast<uint32_t>(m_pipe) << DRM_VBLANK_HIGH_CRTC_SHIFT;
    if (drmWaitVBlank(m_drm_fd, &vbl) != 0)
        VIDEO_DEBUG_PRINT("DRMDisplay: drmWaitVBlank: %s", ::strerror(errno));
}

void DRMDisplay::wait_flip_event(bool &flip_done)
{
    drmEventContext evctx{};
    evctx.version = DRM_EVENT_CONTEXT_VERSION;
    evctx.page_flip_handler = on_page_flip;
    while (!flip_done)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_drm_fd, &fds);
        struct timeval tv{2, 0};
        int n = select(m_drm_fd + 1, &fds, nullptr, nullptr, &tv);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
        {
            VIDEO_DEBUG_PRINT("DRMDisplay: page-flip event timeout");
            break;
        }
        drmHandleEvent(m_drm_fd, &evctx);
    }
}

void DRMDisplay::free_frame(Frame &f, bool call_release)
{
    // Framebuffers are cached and cleaned up in the destructor — just clear the reference.
    f.fb_id = 0;

    if (call_release && f.buf && m_release_cb)
        m_release_cb(f.buf);

    f.buf = nullptr;
}

void DRMDisplay::init_drm()
{
    m_drm_fd = ::open(m_cfg.drm_device.c_str(), O_RDWR | O_CLOEXEC);
    if (m_drm_fd < 0)
        throw std::runtime_error("DRMDisplay: cannot open '" + m_cfg.drm_device + "': " + ::strerror(errno));

    drmSetClientCap(m_drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    auto *res = drmModeGetResources(m_drm_fd);
    if (!res)
    {
        ::close(m_drm_fd);
        m_drm_fd = -1;
        throw std::runtime_error("DRMDisplay: drmModeGetResources failed");
    }

    auto *conn = (m_cfg.connector_id >= 0) ? drmModeGetConnector(m_drm_fd, static_cast<uint32_t>(m_cfg.connector_id))
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
    // conn and res are not needed beyond this point
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    if (!crtc)
    {
        close_drm();
        throw std::runtime_error("DRMDisplay: cannot find CRTC for connector");
    }
    m_crtc_id = crtc->crtc_id;
    m_first_frame = !crtc->mode_valid || m_cfg.force_mode_set;
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
        plane = find_plane_for_crtc(m_drm_fd, pres, m_pipe, DRM_PLANE_TYPE_OVERLAY);
        if (!plane)
            plane = find_plane_for_crtc(m_drm_fd, pres, m_pipe, DRM_PLANE_TYPE_PRIMARY);
    }
    drmModeFreePlaneResources(pres);

    if (!plane)
    {
        close_drm();
        throw std::runtime_error("DRMDisplay: cannot find a suitable plane");
    }
    m_plane_id = plane->plane_id;
    drmModeFreePlane(plane);

    if (drmSetClientCap(m_drm_fd, DRM_CLIENT_CAP_ATOMIC, 1) == 0)
    {
        auto gp = [&](const char *n) { return get_property_id(m_drm_fd, m_plane_id, DRM_MODE_OBJECT_PLANE, n); };
        m_plane_props = {gp("FB_ID"),  gp("CRTC_ID"), gp("CRTC_X"), gp("CRTC_Y"), gp("CRTC_W"),
                         gp("CRTC_H"), gp("SRC_X"),   gp("SRC_Y"),  gp("SRC_W"),  gp("SRC_H")};
        m_atomic = m_plane_props.fb_id && m_plane_props.crtc_id && m_plane_props.crtc_w && m_plane_props.crtc_h &&
                   m_plane_props.src_w && m_plane_props.src_h;
        VIDEO_DEBUG_PRINT("DRMDisplay: atomic modesetting %s",
                          m_atomic ? "enabled" : "unavailable (incomplete plane props)");
    }
    else
    {
        VIDEO_DEBUG_PRINT("DRMDisplay: DRM_CLIENT_CAP_ATOMIC not supported, using legacy SetPlane");
    }

    VIDEO_DEBUG_PRINT("DRMDisplay: fd=%d connector=%u crtc=%u plane=%u pipe=%d atomic=%d", m_drm_fd, m_conn_id,
                      m_crtc_id, m_plane_id, m_pipe, m_atomic);
}

void DRMDisplay::close_drm()
{
    if (m_drm_fd >= 0)
    {
        ::close(m_drm_fd);
        m_drm_fd = -1;
    }
}

void DRMDisplay::show(AL_TBuffer *frame, const AL_TInfoDecode &info)
{
    if (m_stopped.load())
    {
        VIDEO_ERROR_PRINT("DRMDisplay::show: display stopped, releasing frame");
        if (m_release_cb)
            m_release_cb(frame);
        return;
    }
    if (!frame)
    {
        VIDEO_ERROR_PRINT("DRMDisplay::show: null frame");
        return;
    }
    auto *meta = reinterpret_cast<AL_TPixMapMetaData *>(AL_Buffer_GetMetaData(frame, AL_META_TYPE_PIXMAP));
    if (!meta)
    {
        VIDEO_ERROR_PRINT("DRMDisplay::show: no AL_TPixMapMetaData");
        return;
    }
    const uint32_t drm_fmt = allegro_to_drm_fourcc(meta->tFourCC);
    if (!drm_fmt)
    {
        VIDEO_ERROR_PRINT("DRMDisplay::show: unsupported pixel format 0x%08X", meta->tFourCC);
        return;
    }

    PendingFrame pf{frame, static_cast<uint32_t>(info.tDim.iWidth), static_cast<uint32_t>(info.tDim.iHeight)};
    submit_frame(std::move(pf));
}

bool DRMDisplay::create_fb(AL_TBuffer *buf, uint32_t w, uint32_t h, uint32_t &out_fb_id)
{
    // Check cache first (typical path after warmup)
    {
        std::lock_guard<std::mutex> lk(m_cache_mutex);
        auto it = m_fb_cache.find(buf);
        if (it != m_fb_cache.end())
        {
            out_fb_id = it->second.fb_id;
            return true;
        }
    }

    // Cache miss: import DMA-buf and create framebuffer
    auto *meta = reinterpret_cast<AL_TPixMapMetaData *>(AL_Buffer_GetMetaData(buf, AL_META_TYPE_PIXMAP));
    if (!meta)
    {
        VIDEO_ERROR_PRINT("DRMDisplay::create_fb: no pixmap metadata");
        return false;
    }
    const uint32_t drm_fmt = allegro_to_drm_fourcc(meta->tFourCC);
    if (!drm_fmt)
    {
        VIDEO_ERROR_PRINT("DRMDisplay::create_fb: unsupported pixel format 0x%08X", meta->tFourCC);
        return false;
    }

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

        int gi = -1;
        for (int k = 0; k < num_gem; ++k)
        {
            if (imported_chunk[k] == pl.iChunkIdx)
            {
                gi = k;
                break;
            }
        }

        if (gi < 0)
        {
            int dma_fd = AL_LinuxDmaAllocator_GetFd(linux_alloc, buf->hBufs[pl.iChunkIdx]);
            if (dma_fd < 0)
            {
                VIDEO_ERROR_PRINT("DRMDisplay::create_fb: cannot get DMA-buf fd for chunk %d", pl.iChunkIdx);
                goto cleanup;
            }
            uint32_t gem = 0;
            if (drmPrimeFDToHandle(m_drm_fd, dma_fd, &gem) != 0)
            {
                VIDEO_ERROR_PRINT("DRMDisplay::create_fb: drmPrimeFDToHandle: %s", ::strerror(errno));
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
            VIDEO_ERROR_PRINT("DRMDisplay::create_fb: drmModeAddFB2: %s (%d)", ::strerror(errno), errno);
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