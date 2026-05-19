/**
 * @file DRMDisplay.cpp
 * @brief Zero-copy DRM/KMS display for Xilinx ZYNQ VCU decoded frames.
 *
 * Implementation mirrors the logic of the GStreamer kmssink plugin:
 *  - PRIME DMA-buf import via drmPrimeFDToHandle
 *  - Framebuffer creation via drmModeAddFB2
 *  - Plane update via drmModeSetPlane
 *  - Optional vsync wait via drmModePageFlip + drmHandleEvent
 *
 * Unlike kmssink, this module is not GStreamer-based and is designed to be
 * called directly from the Allegro DVT decode SDK callback thread.
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
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>

// ---------------------------------------------------------------------------
// Xilinx-specific DRM format codes (not in all drm_fourcc.h versions)
// ---------------------------------------------------------------------------
#ifndef DRM_FORMAT_XV15
// 2×2 subsampled Cr:Cb plane, 2:10:10:10 packing  (NV12 10-bit, Xilinx)
#define DRM_FORMAT_XV15 fourcc_code('X', 'V', '1', '5')
#endif
#ifndef DRM_FORMAT_XV20
// 2×1 subsampled Cr:Cb plane, 2:10:10:10 packing  (NV16 10-bit, Xilinx)
#define DRM_FORMAT_XV20 fourcc_code('X', 'V', '2', '0')
#endif

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
namespace
{

/// Return the DRM fourcc for the given Allegro SDK fourcc, or 0 if unknown.
uint32_t allegro_to_drm_fourcc(uint32_t al_fourcc)
{
    // Allegro fourchars are defined as FOURCC(XXXX) which encodes the same
    // byte order as DRM fourcc codes.
    switch (al_fourcc)
    {
    case FOURCC(NV12):
        return DRM_FORMAT_NV12;
    case FOURCC(XV15):
        return DRM_FORMAT_XV15; // 10-bit 4:2:0, Xilinx packed
    case FOURCC(NV16):
        return DRM_FORMAT_NV16;
    case FOURCC(XV20):
        return DRM_FORMAT_XV20; // 10-bit 4:2:2, Xilinx packed
    case FOURCC(NV24):
        return DRM_FORMAT_NV24;
    default:
        return 0;
    }
}

/// Find the first connected DRM connector.
drmModeConnector *find_first_connected_connector(int fd, drmModeRes *res)
{
    for (int i = 0; i < res->count_connectors; ++i)
    {
        auto *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED && conn->count_modes > 0)
            return conn;
        if (conn)
            drmModeFreeConnector(conn);
    }
    return nullptr;
}

/// Find an active CRTC for the given connector; also sets *pipe (0-based).
drmModeCrtc *find_crtc_for_connector(int fd, drmModeRes *res, drmModeConnector *conn, int *pipe)
{
    // First, try the connector's currently active encoder.
    for (int e = 0; e < conn->count_encoders; ++e)
    {
        auto *enc = drmModeGetEncoder(fd, conn->encoders[e]);
        if (!enc)
            continue;
        if (enc->crtc_id)
        {
            for (int c = 0; c < res->count_crtcs; ++c)
            {
                if (res->crtcs[c] == enc->crtc_id)
                {
                    auto *crtc = drmModeGetCrtc(fd, enc->crtc_id);
                    drmModeFreeEncoder(enc);
                    if (pipe)
                        *pipe = c;
                    return crtc;
                }
            }
        }
        // Fallback: pick first compatible CRTC reported by encoder.
        if (enc->possible_crtcs)
        {
            int idx = __builtin_ctz(enc->possible_crtcs); // lowest set bit
            if (idx < res->count_crtcs)
            {
                auto *crtc = drmModeGetCrtc(fd, res->crtcs[idx]);
                drmModeFreeEncoder(enc);
                if (pipe)
                    *pipe = idx;
                return crtc;
            }
        }
        drmModeFreeEncoder(enc);
    }
    return nullptr;
}

/// Find a suitable overlay (or primary) plane for the given CRTC pipe.
drmModePlane *find_plane_for_crtc(int fd, drmModePlaneRes *pres, int pipe, int prefer_type)
{
    drmModePlane *fallback = nullptr;

    for (uint32_t i = 0; i < pres->count_planes; ++i)
    {
        auto *plane = drmModeGetPlane(fd, pres->planes[i]);
        if (!plane)
            continue;
        if (!(plane->possible_crtcs & (1u << pipe)))
        {
            drmModeFreePlane(plane);
            continue;
        }
        // Query the "type" property of this plane.
        auto *props = drmModeObjectGetProperties(fd, plane->plane_id, DRM_MODE_OBJECT_PLANE);
        int plane_type = -1;
        if (props)
        {
            for (uint32_t p = 0; p < props->count_props; ++p)
            {
                auto *prop = drmModeGetProperty(fd, props->props[p]);
                if (prop && std::string(prop->name) == "type")
                    plane_type = static_cast<int>(props->prop_values[p]);
                if (prop)
                    drmModeFreeProperty(prop);
            }
            drmModeFreeObjectProperties(props);
        }
        if (plane_type == prefer_type)
        {
            if (fallback)
                drmModeFreePlane(fallback);
            return plane;
        }
        if (!fallback)
            fallback = plane;
        else
            drmModeFreePlane(plane);
    }
    return fallback;
}

struct FlipPending
{
    bool done;
};

static void page_flip_handler(int /*fd*/, unsigned int /*seq*/, unsigned int /*sec*/, unsigned int /*usec*/, void *data)
{
    static_cast<FlipPending *>(data)->done = true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// DRMDisplay implementation
// ---------------------------------------------------------------------------

DRMDisplay::DRMDisplay(const DRMDisplayConfig &cfg) : m_cfg(cfg)
{
    init_drm();
}

DRMDisplay::~DRMDisplay()
{
    release_current();
    close_drm();
}

// ---------------------------------------------------------------------------
void DRMDisplay::init_drm()
{
    // Open device by path. drmOpen() takes a driver name, not a path;
    // for device-path opening the correct approach is a plain open() call.
    m_drm_fd = ::open(m_cfg.drm_device.c_str(), O_RDWR | O_CLOEXEC);
    if (m_drm_fd < 0)
    {
        throw std::runtime_error("DRMDisplay: cannot open DRM device '" + m_cfg.drm_device + "': " + ::strerror(errno));
    }

    // Require PRIME import support.
    uint64_t prime_caps = 0;
    drmGetCap(m_drm_fd, DRM_CAP_PRIME, &prime_caps);
    if (!(prime_caps & DRM_PRIME_CAP_IMPORT))
    {
        drmClose(m_drm_fd);
        m_drm_fd = -1;
        throw std::runtime_error("DRMDisplay: DRM driver does not support PRIME import");
    }

    // Enable universal planes so we can enumerate overlay planes.
    drmSetClientCap(m_drm_fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    auto *res = drmModeGetResources(m_drm_fd);
    if (!res)
    {
        drmClose(m_drm_fd);
        m_drm_fd = -1;
        throw std::runtime_error("DRMDisplay: drmModeGetResources failed");
    }

    // --- Connector ---
    drmModeConnector *conn = nullptr;
    if (m_cfg.connector_id >= 0)
    {
        conn = drmModeGetConnector(m_drm_fd, static_cast<uint32_t>(m_cfg.connector_id));
        if (!conn)
        {
            drmModeFreeResources(res);
            drmClose(m_drm_fd);
            m_drm_fd = -1;
            throw std::runtime_error("DRMDisplay: connector " + std::to_string(m_cfg.connector_id) + " not found");
        }
    }
    else
    {
        conn = find_first_connected_connector(m_drm_fd, res);
        if (!conn)
        {
            drmModeFreeResources(res);
            drmClose(m_drm_fd);
            m_drm_fd = -1;
            throw std::runtime_error("DRMDisplay: no connected connector found");
        }
    }
    m_conn_id = conn->connector_id;

    // --- CRTC ---
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
    if (!crtc)
    {
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        drmClose(m_drm_fd);
        m_drm_fd = -1;
        throw std::runtime_error("DRMDisplay: cannot find CRTC for connector");
    }
    m_crtc_id = crtc->crtc_id;

    // Remember whether a mode is already active (to decide about mode-set later).
    bool mode_active = crtc->mode_valid;
    drmModeFreeCrtc(crtc);
    if (!mode_active)
        m_first_frame = true; // will trigger mode-set on first show()
    else
        m_first_frame = m_cfg.force_mode_set;

    // --- Plane ---
    auto *pres = drmModeGetPlaneResources(m_drm_fd);
    if (!pres)
    {
        drmModeFreeConnector(conn);
        drmModeFreeResources(res);
        drmClose(m_drm_fd);
        m_drm_fd = -1;
        throw std::runtime_error("DRMDisplay: drmModeGetPlaneResources failed");
    }

    drmModePlane *plane = nullptr;
    if (m_cfg.plane_id >= 0)
    {
        plane = drmModeGetPlane(m_drm_fd, static_cast<uint32_t>(m_cfg.plane_id));
    }
    else
    {
        // Prefer overlay plane, fall back to primary.
        plane = find_plane_for_crtc(m_drm_fd, pres, m_pipe, DRM_PLANE_TYPE_OVERLAY);
        if (!plane)
            plane = find_plane_for_crtc(m_drm_fd, pres, m_pipe, DRM_PLANE_TYPE_PRIMARY);
    }
    drmModeFreePlaneResources(pres);
    drmModeFreeConnector(conn);
    drmModeFreeResources(res);

    if (!plane)
    {
        drmClose(m_drm_fd);
        m_drm_fd = -1;
        throw std::runtime_error("DRMDisplay: cannot find a suitable plane for the CRTC");
    }
    m_plane_id = plane->plane_id;
    drmModeFreePlane(plane);

    VIDEO_DEBUG_PRINT("DRMDisplay: fd=%d connector=%u crtc=%u plane=%u pipe=%d", m_drm_fd, m_conn_id, m_crtc_id,
                      m_plane_id, m_pipe);
}

// ---------------------------------------------------------------------------
void DRMDisplay::close_drm()
{
    if (m_drm_fd >= 0)
    {
        ::close(m_drm_fd);
        m_drm_fd = -1;
    }
}

uint32_t DRMDisplay::allegro_fourcc_to_drm(uint32_t al_fourcc) const
{
    return ::allegro_to_drm_fourcc(al_fourcc);
}

// ---------------------------------------------------------------------------
void DRMDisplay::free_held_frame(HeldFrame &f)
{
    if (f.fb_id)
    {
        drmModeRmFB(m_drm_fd, f.fb_id);
        f.fb_id = 0;
    }
    for (int i = 0; i < f.num_gem; ++i)
    {
        if (f.gem_handles[i])
        {
            struct drm_gem_close close_arg{};
            close_arg.handle = f.gem_handles[i];
            drmIoctl(m_drm_fd, DRM_IOCTL_GEM_CLOSE, &close_arg);
            f.gem_handles[i] = 0;
        }
    }
    f.num_gem = 0;
    if (f.on_return)
    {
        f.on_return();
        f.on_return = nullptr;
    }
    f.buf = nullptr;
}

// ---------------------------------------------------------------------------
void DRMDisplay::release_current()
{
    free_held_frame(m_held);
}

// ---------------------------------------------------------------------------
bool DRMDisplay::do_set_plane(uint32_t fb_id, uint32_t w, uint32_t h)
{
    int ret = drmModeSetPlane(m_drm_fd, m_plane_id, m_crtc_id, fb_id, 0, 0, 0, w, h, // dst: x,y,w,h  (display area)
                              0, 0, w << 16, h << 16 // src: x,y,w,h  (fixed-point 16.16)
    );
    if (ret)
    {
        VIDEO_ERROR_PRINT("DRMDisplay: drmModeSetPlane failed: %s (%d)", ::strerror(errno), errno);
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
bool DRMDisplay::wait_for_flip()
{
    FlipPending pending{false};
    int ret = drmModePageFlip(m_drm_fd, m_crtc_id, m_held.fb_id, DRM_MODE_PAGE_FLIP_EVENT, &pending);
    if (ret)
    {
        VIDEO_DEBUG_PRINT("DRMDisplay: drmModePageFlip not available (%d), skipping vsync", ret);
        return true; // non-fatal
    }

    drmEventContext evctx{};
    evctx.version = DRM_EVENT_CONTEXT_VERSION;
    evctx.page_flip_handler = page_flip_handler;

    while (!pending.done)
    {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(m_drm_fd, &fds);
        struct timeval tv{1, 0}; // 1-second timeout guard
        int n = select(m_drm_fd + 1, &fds, nullptr, nullptr, &tv);
        if (n <= 0)
            break;
        drmHandleEvent(m_drm_fd, &evctx);
    }
    return true;
}

// ---------------------------------------------------------------------------
bool DRMDisplay::show(AL_TBuffer *frame, const AL_TInfoDecode &info, FrameReturnCallback on_frame_return)
{
    if (!frame || !on_frame_return)
    {
        VIDEO_ERROR_PRINT("DRMDisplay::show: null frame or callback");
        return false;
    }

    // -----------------------------------------------------------------------
    // 1. Extract plane layout from the PixMap metadata.
    // -----------------------------------------------------------------------
    auto *pixmap_meta = reinterpret_cast<AL_TPixMapMetaData *>(AL_Buffer_GetMetaData(frame, AL_META_TYPE_PIXMAP));
    if (!pixmap_meta)
    {
        VIDEO_ERROR_PRINT("DRMDisplay::show: AL_TBuffer has no AL_TPixMapMetaData");
        return false;
    }

    uint32_t drm_fmt = allegro_to_drm_fourcc(pixmap_meta->tFourCC);
    if (!drm_fmt)
    {
        VIDEO_ERROR_PRINT("DRMDisplay::show: unsupported pixel format 0x%08X", pixmap_meta->tFourCC);
        return false;
    }

    const uint32_t w = static_cast<uint32_t>(info.tDim.iWidth);
    const uint32_t h = static_cast<uint32_t>(info.tDim.iHeight);

    // Gather plane descriptors: each AL_TPlane carries the chunk index,
    // byte offset within that chunk, and pitch.
    // For semi-planar formats the SDK packs Y + UV into a single DMA chunk
    // (chunk index 0 for both), so we only ever need one DMA-buf fd.
    static const AL_EPlaneId kSemiPlanarIds[2] = {AL_PLANE_Y, AL_PLANE_UV};
    static const AL_EPlaneId kYUVPlanarIds[3] = {AL_PLANE_Y, AL_PLANE_U, AL_PLANE_V};
    const AL_EPlaneId *plane_ids = kSemiPlanarIds;
    int num_planes = 2;

    if (AL_GetChromaMode(pixmap_meta->tFourCC) == AL_CHROMA_MONO)
    {
        num_planes = 1;
    }

    // -----------------------------------------------------------------------
    // 2. Import DMA-buf fd(s) into DRM GEM handles.
    // -----------------------------------------------------------------------
    // Since the rec pool is created with a single chunk for all planes,
    // hBufs[0] holds the DMA-buf for the entire frame.  For formats where
    // Y and UV share one buffer, both planes reference chunk index 0.
    uint32_t bo_handles[4] = {};
    uint32_t pitches[4] = {};
    uint32_t offsets[4] = {};
    uint32_t gem_handles[4] = {};
    int num_gem = 0;

    // Track which chunk indices we have already imported (avoid duplicates).
    int imported_chunk[4] = {-1, -1, -1, -1};

    for (int pi = 0; pi < num_planes; ++pi)
    {
        const AL_TPlane &pl = pixmap_meta->tPlanes[plane_ids[pi]];
        pitches[pi] = static_cast<uint32_t>(pl.iPitch);
        offsets[pi] = static_cast<uint32_t>(pl.iOffset);

        // Look up whether we have already imported this chunk.
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
            // New chunk: get the DMA-buf fd and import it.
            auto *linux_alloc = reinterpret_cast<AL_TLinuxDmaAllocator *>(frame->pAllocator);
            int dma_fd = AL_LinuxDmaAllocator_GetFd(linux_alloc, frame->hBufs[pl.iChunkIdx]);
            if (dma_fd < 0)
            {
                VIDEO_ERROR_PRINT("DRMDisplay::show: failed to get DMA-buf fd for chunk %d", pl.iChunkIdx);
                // Close any GEM handles opened so far.
                for (int k = 0; k < num_gem; ++k)
                {
                    struct drm_gem_close ca{};
                    ca.handle = gem_handles[k];
                    drmIoctl(m_drm_fd, DRM_IOCTL_GEM_CLOSE, &ca);
                }
                return false;
            }

            uint32_t gem = 0;
            if (drmPrimeFDToHandle(m_drm_fd, dma_fd, &gem) != 0)
            {
                VIDEO_ERROR_PRINT("DRMDisplay::show: drmPrimeFDToHandle failed: %s", ::strerror(errno));
                for (int k = 0; k < num_gem; ++k)
                {
                    struct drm_gem_close ca{};
                    ca.handle = gem_handles[k];
                    drmIoctl(m_drm_fd, DRM_IOCTL_GEM_CLOSE, &ca);
                }
                return false;
            }

            gi = num_gem;
            gem_handles[num_gem] = gem;
            imported_chunk[num_gem] = pl.iChunkIdx;
            ++num_gem;
        }

        bo_handles[pi] = gem_handles[gi];
    }

    // -----------------------------------------------------------------------
    // 3. Create DRM framebuffer.
    // -----------------------------------------------------------------------
    uint32_t fb_id = 0;
    if (drmModeAddFB2(m_drm_fd, w, h, drm_fmt, bo_handles, pitches, offsets, &fb_id, 0) != 0)
    {
        VIDEO_ERROR_PRINT("DRMDisplay::show: drmModeAddFB2 failed: %s (%d)", ::strerror(errno), errno);
        for (int k = 0; k < num_gem; ++k)
        {
            struct drm_gem_close ca{};
            ca.handle = gem_handles[k];
            drmIoctl(m_drm_fd, DRM_IOCTL_GEM_CLOSE, &ca);
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // 4. First frame: set CRTC mode if needed.
    // -----------------------------------------------------------------------
    if (m_first_frame)
    {
        // Query current mode from the CRTC; if valid we keep it.
        // For headless or mode-less CRTCs the caller should set force_mode_set.
        auto *crtc = drmModeGetCrtc(m_drm_fd, m_crtc_id);
        if (crtc && crtc->mode_valid && !m_cfg.force_mode_set)
        {
            // Re-use existing mode; just bind the new fb to the CRTC once.
            int err = drmModeSetCrtc(m_drm_fd, m_crtc_id, fb_id, 0, 0, &m_conn_id, 1, &crtc->mode);
            if (err)
                VIDEO_DEBUG_PRINT("DRMDisplay: drmModeSetCrtc: %s (non-fatal)", ::strerror(errno));
        }
        if (crtc)
            drmModeFreeCrtc(crtc);
        m_first_frame = false;
    }

    // -----------------------------------------------------------------------
    // 5. Present the frame on the overlay plane.
    // -----------------------------------------------------------------------
    if (!do_set_plane(fb_id, w, h))
    {
        drmModeRmFB(m_drm_fd, fb_id);
        for (int k = 0; k < num_gem; ++k)
        {
            struct drm_gem_close ca{};
            ca.handle = gem_handles[k];
            drmIoctl(m_drm_fd, DRM_IOCTL_GEM_CLOSE, &ca);
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // 6. Build the new HeldFrame record, then release the previous one.
    //    Order matters: release AFTER SetPlane so the display never goes dark.
    // -----------------------------------------------------------------------
    HeldFrame new_frame{};
    new_frame.buf = frame;
    new_frame.fb_id = fb_id;
    for (int k = 0; k < num_gem; ++k)
        new_frame.gem_handles[k] = gem_handles[k];
    new_frame.num_gem = num_gem;
    new_frame.on_return = std::move(on_frame_return);

    // Release the frame that was displayed until now.
    free_held_frame(m_held);

    // -----------------------------------------------------------------------
    // 7. Optionally wait for vsync.
    // -----------------------------------------------------------------------
    m_held = std::move(new_frame);
    if (m_cfg.enable_vsync)
        wait_for_flip();

    return true;
}