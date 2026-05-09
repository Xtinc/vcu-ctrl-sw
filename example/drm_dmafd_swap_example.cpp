#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <getopt.h>
#include <linux/dma-heap.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

extern "C"
{
#include <drm/drm_mode.h>
#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
}

#include <iostream>
#include <string>
#include <vector>

namespace
{
struct DmaFrame
{
    int dma_fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    size_t size = 0;
    void *map = MAP_FAILED;
    uint32_t gem_handle = 0;
    uint32_t fb_id = 0;
};

struct DrmSetup
{
    int card_fd = -1;
    drmModeModeInfo mode{};
    uint32_t conn_id = 0;
    uint32_t crtc_id = 0;
    uint32_t plane_id = 0;
    uint32_t crtc_index = 0;
    uint32_t old_crtc_id = 0;
    drmModeCrtc *saved_crtc = nullptr;
};

static uint32_t fourcc_to_u32(const char *s)
{
    return static_cast<uint32_t>(s[0]) | (static_cast<uint32_t>(s[1]) << 8U) |
           (static_cast<uint32_t>(s[2]) << 16U) | (static_cast<uint32_t>(s[3]) << 24U);
}

static void print_usage(const char *prog)
{
    std::cout << "Usage: " << prog << " [options]\n"
              << "  --card <path>      DRM card path (default: /dev/dri/card0)\n"
              << "  --heap <path>      dma-heap path (default: /dev/dma_heap/linux,cma)\n"
              << "  --width <n>        Frame width (default: 1280)\n"
              << "  --height <n>       Frame height (default: 720)\n"
              << "  --count <n>        Number of DMA frames (default: 3)\n"
              << "  --fps <n>          Display fps (default: 2)\n"
              << "  --seconds <n>      Run duration (default: 30)\n"
              << "  --help             Show help\n\n"
              << "This demo allocates multiple DMA-BUFs, paints each with a different pattern,\n"
              << "imports them into DRM and flips between framebuffers (FD zero-copy scanout).\n";
}

static bool alloc_dma_buffer(const std::string &heap_path, uint32_t width, uint32_t height, DmaFrame &out)
{
    const uint32_t stride = width * 4U;
    const size_t size = static_cast<size_t>(stride) * height;

    int heap_fd = open(heap_path.c_str(), O_RDWR | O_CLOEXEC);
    if (heap_fd < 0)
    {
        std::cerr << "open(" << heap_path << ") failed: " << strerror(errno) << "\n";
        return false;
    }

    dma_heap_allocation_data alloc{};
    alloc.len = size;
    alloc.fd_flags = O_RDWR | O_CLOEXEC;
    alloc.heap_flags = 0;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0)
    {
        std::cerr << "DMA_HEAP_IOCTL_ALLOC failed: " << strerror(errno) << "\n";
        close(heap_fd);
        return false;
    }
    close(heap_fd);

    void *map = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, alloc.fd, 0);
    if (map == MAP_FAILED)
    {
        std::cerr << "mmap dma fd failed: " << strerror(errno) << "\n";
        close(alloc.fd);
        return false;
    }

    out.dma_fd = alloc.fd;
    out.width = width;
    out.height = height;
    out.stride = stride;
    out.size = size;
    out.map = map;
    return true;
}

static bool alloc_dma_buffer_with_fallback(const std::string &heap_path, uint32_t width, uint32_t height, DmaFrame &out)
{
    std::vector<std::string> candidates;
    candidates.push_back(heap_path);

    if (heap_path != "/dev/dma_heap/linux,cma")
    {
        candidates.push_back("/dev/dma_heap/linux,cma");
    }
    if (heap_path != "/dev/dma_heap/system")
    {
        candidates.push_back("/dev/dma_heap/system");
    }

    for (const auto &heap : candidates)
    {
        if (alloc_dma_buffer(heap, width, height, out))
        {
            std::cout << "Using dma-heap: " << heap << "\n";
            return true;
        }
    }

    return false;
}

static void free_dma_buffer(DmaFrame &buf)
{
    if (buf.map != MAP_FAILED)
    {
        munmap(buf.map, buf.size);
        buf.map = MAP_FAILED;
    }
    if (buf.fb_id != 0)
    {
        // fb is removed by DRM fd owner in caller.
        buf.fb_id = 0;
    }
    if (buf.gem_handle != 0)
    {
        buf.gem_handle = 0;
    }
    if (buf.dma_fd >= 0)
    {
        close(buf.dma_fd);
        buf.dma_fd = -1;
    }
}

static void paint_pattern(DmaFrame &frame, uint32_t index)
{
    uint8_t *p = reinterpret_cast<uint8_t *>(frame.map);
    if (!p)
    {
        return;
    }

    dma_buf_sync sync_start{};
    sync_start.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
    if (ioctl(frame.dma_fd, DMA_BUF_IOCTL_SYNC, &sync_start) != 0)
    {
        std::cerr << "DMA_BUF_IOCTL_SYNC start failed: " << strerror(errno) << "\n";
    }

    const uint8_t r = static_cast<uint8_t>((index * 73U) & 0xFFU);
    const uint8_t g = static_cast<uint8_t>((index * 151U + 64U) & 0xFFU);
    const uint8_t b = static_cast<uint8_t>((index * 199U + 128U) & 0xFFU);

    for (uint32_t y = 0; y < frame.height; ++y)
    {
        uint32_t *row = reinterpret_cast<uint32_t *>(p + static_cast<size_t>(y) * frame.stride);
        for (uint32_t x = 0; x < frame.width; ++x)
        {
            uint8_t rr = static_cast<uint8_t>((r + x / 8U) & 0xFFU);
            uint8_t gg = static_cast<uint8_t>((g + y / 8U) & 0xFFU);
            uint8_t bb = b;
            row[x] = (0xFFU << 24U) | (static_cast<uint32_t>(rr) << 16U) | (static_cast<uint32_t>(gg) << 8U) |
                     bb;
        }
    }

    msync(frame.map, frame.size, MS_SYNC);

    dma_buf_sync sync_end{};
    sync_end.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
    if (ioctl(frame.dma_fd, DMA_BUF_IOCTL_SYNC, &sync_end) != 0)
    {
        std::cerr << "DMA_BUF_IOCTL_SYNC end failed: " << strerror(errno) << "\n";
    }
}

static bool pick_connector_crtc(DrmSetup &s)
{
    drmModeRes *res = drmModeGetResources(s.card_fd);
    if (!res)
    {
        std::cerr << "drmModeGetResources failed\n";
        return false;
    }

    bool ok = false;

    for (int i = 0; i < res->count_connectors && !ok; ++i)
    {
        drmModeConnector *conn = drmModeGetConnector(s.card_fd, res->connectors[i]);
        if (!conn)
        {
            continue;
        }

        if (conn->connection != DRM_MODE_CONNECTED || conn->count_modes == 0)
        {
            drmModeFreeConnector(conn);
            continue;
        }

        drmModeEncoder *enc = nullptr;
        if (conn->encoder_id != 0)
        {
            enc = drmModeGetEncoder(s.card_fd, conn->encoder_id);
        }

        uint32_t crtc_id = 0;
        uint32_t crtc_index = 0;

        if (enc)
        {
            crtc_id = enc->crtc_id;
            for (int c = 0; c < res->count_crtcs; ++c)
            {
                if (res->crtcs[c] == crtc_id)
                {
                    crtc_index = static_cast<uint32_t>(c);
                    break;
                }
            }
        }
        else
        {
            for (int e = 0; e < conn->count_encoders && crtc_id == 0; ++e)
            {
                drmModeEncoder *alt = drmModeGetEncoder(s.card_fd, conn->encoders[e]);
                if (!alt)
                {
                    continue;
                }
                for (int c = 0; c < res->count_crtcs; ++c)
                {
                    if ((alt->possible_crtcs & (1 << c)) != 0)
                    {
                        crtc_id = res->crtcs[c];
                        crtc_index = static_cast<uint32_t>(c);
                        break;
                    }
                }
                drmModeFreeEncoder(alt);
            }
        }

        if (crtc_id == 0)
        {
            if (enc)
            {
                drmModeFreeEncoder(enc);
            }
            drmModeFreeConnector(conn);
            continue;
        }

        s.conn_id = conn->connector_id;
        s.crtc_id = crtc_id;
        s.crtc_index = crtc_index;
        s.mode = conn->modes[0];
        ok = true;

        if (enc)
        {
            drmModeFreeEncoder(enc);
        }
        drmModeFreeConnector(conn);
    }

    drmModeFreeResources(res);
    return ok;
}

static bool import_fb(DrmSetup &s, DmaFrame &f)
{
    if (drmPrimeFDToHandle(s.card_fd, f.dma_fd, &f.gem_handle) != 0)
    {
        std::cerr << "drmPrimeFDToHandle failed: " << strerror(errno) << "\n";
        return false;
    }

    uint32_t handles[4] = {f.gem_handle, 0, 0, 0};
    uint32_t pitches[4] = {f.stride, 0, 0, 0};
    uint32_t offsets[4] = {0, 0, 0, 0};

    int ret = drmModeAddFB2(s.card_fd, f.width, f.height, DRM_FORMAT_XRGB8888, handles, pitches, offsets, &f.fb_id, 0);
    if (ret != 0)
    {
        std::cerr << "drmModeAddFB2 failed: " << strerror(errno) << "\n";
        return false;
    }

    return true;
}

static bool modeset_first_fb(DrmSetup &s, uint32_t fb_id)
{
    s.saved_crtc = drmModeGetCrtc(s.card_fd, s.crtc_id);
    if (!s.saved_crtc)
    {
        std::cerr << "drmModeGetCrtc failed\n";
    }

    int ret = drmModeSetCrtc(s.card_fd, s.crtc_id, fb_id, 0, 0, &s.conn_id, 1, &s.mode);
    if (ret != 0)
    {
        std::cerr << "drmModeSetCrtc failed: " << strerror(errno) << "\n";
        return false;
    }
    return true;
}

static void page_flip_handler(int, unsigned int, unsigned int, unsigned int, void *data)
{
    bool *done = reinterpret_cast<bool *>(data);
    if (done)
    {
        *done = true;
    }
}

static bool queue_page_flip_and_wait(DrmSetup &s, uint32_t fb_id)
{
    bool done = false;
    int ret = drmModePageFlip(s.card_fd, s.crtc_id, fb_id, DRM_MODE_PAGE_FLIP_EVENT, &done);
    if (ret != 0)
    {
        std::cerr << "drmModePageFlip failed: " << strerror(errno) << "\n";
        return false;
    }

    drmEventContext ev{};
    ev.version = DRM_EVENT_CONTEXT_VERSION;
    ev.page_flip_handler = page_flip_handler;

    while (!done)
    {
        pollfd pfd{};
        pfd.fd = s.card_fd;
        pfd.events = POLLIN;

        const int pret = poll(&pfd, 1, 2000);
        if (pret <= 0)
        {
            std::cerr << "poll DRM event failed/timeout\n";
            return false;
        }

        if ((pfd.revents & POLLIN) != 0)
        {
            if (drmHandleEvent(s.card_fd, &ev) != 0)
            {
                std::cerr << "drmHandleEvent failed\n";
                return false;
            }
        }
    }

    return true;
}

static void cleanup_drm(DrmSetup &s)
{
    if (s.saved_crtc)
    {
        drmModeSetCrtc(s.card_fd, s.saved_crtc->crtc_id, s.saved_crtc->buffer_id, s.saved_crtc->x, s.saved_crtc->y,
                       &s.conn_id, 1, &s.saved_crtc->mode);
        drmModeFreeCrtc(s.saved_crtc);
        s.saved_crtc = nullptr;
    }

    if (s.card_fd >= 0)
    {
        close(s.card_fd);
        s.card_fd = -1;
    }
}
} // namespace

int main(int argc, char **argv)
{
    std::string card = "/dev/dri/card0";
    std::string heap = "/dev/dma_heap/linux,cma";
    uint32_t width = 1280;
    uint32_t height = 720;
    uint32_t count = 3;
    uint32_t fps = 2;
    uint32_t seconds = 30;

    static option long_opts[] = {
        {"card", required_argument, nullptr, 'c'},   {"heap", required_argument, nullptr, 'm'},
        {"width", required_argument, nullptr, 'w'},  {"height", required_argument, nullptr, 'h'},
        {"count", required_argument, nullptr, 'n'},  {"fps", required_argument, nullptr, 'f'},
        {"seconds", required_argument, nullptr, 's'}, {"help", no_argument, nullptr, '?'},
        {nullptr, 0, nullptr, 0}};

    int opt = 0;
    while ((opt = getopt_long(argc, argv, "", long_opts, nullptr)) != -1)
    {
        switch (opt)
        {
        case 'c':
            card = optarg;
            break;
        case 'm':
            heap = optarg;
            break;
        case 'w':
            width = static_cast<uint32_t>(std::stoul(optarg));
            break;
        case 'h':
            height = static_cast<uint32_t>(std::stoul(optarg));
            break;
        case 'n':
            count = static_cast<uint32_t>(std::stoul(optarg));
            break;
        case 'f':
            fps = static_cast<uint32_t>(std::stoul(optarg));
            break;
        case 's':
            seconds = static_cast<uint32_t>(std::stoul(optarg));
            break;
        default:
            print_usage(argv[0]);
            return 0;
        }
    }

    if (count < 2)
    {
        std::cerr << "count must be >= 2 for visible frame replacement\n";
        return 1;
    }

    if (fps == 0)
    {
        std::cerr << "fps must be > 0\n";
        return 1;
    }

    DrmSetup drm{};
    drm.card_fd = open(card.c_str(), O_RDWR | O_CLOEXEC);
    if (drm.card_fd < 0)
    {
        std::cerr << "open(" << card << ") failed: " << strerror(errno) << "\n";
        return 1;
    }

    if (!pick_connector_crtc(drm))
    {
        std::cerr << "No connected DRM connector/crtc found\n";
        cleanup_drm(drm);
        return 1;
    }

    if (width == 0 || height == 0)
    {
        width = static_cast<uint32_t>(drm.mode.hdisplay);
        height = static_cast<uint32_t>(drm.mode.vdisplay);
    }

    std::cout << "DRM picked connector=" << drm.conn_id << " crtc=" << drm.crtc_id << " mode=" << drm.mode.hdisplay
              << "x" << drm.mode.vdisplay << "@" << drm.mode.vrefresh << "\n";

    std::vector<DmaFrame> frames(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        if (!alloc_dma_buffer_with_fallback(heap, width, height, frames[i]))
        {
            std::cerr << "DMA alloc failed at index " << i << "\n";
            for (auto &f : frames)
            {
                free_dma_buffer(f);
            }
            cleanup_drm(drm);
            return 1;
        }

        paint_pattern(frames[i], i + 1);

        if (!import_fb(drm, frames[i]))
        {
            std::cerr << "Import DRM FB failed at index " << i << "\n";
            for (auto &f : frames)
            {
                if (f.fb_id != 0)
                {
                    drmModeRmFB(drm.card_fd, f.fb_id);
                }
                if (f.gem_handle != 0)
                {
                    drm_gem_close gc{};
                    gc.handle = f.gem_handle;
                    ioctl(drm.card_fd, DRM_IOCTL_GEM_CLOSE, &gc);
                }
                free_dma_buffer(f);
            }
            cleanup_drm(drm);
            return 1;
        }
    }

    if (!modeset_first_fb(drm, frames[0].fb_id))
    {
        for (auto &f : frames)
        {
            if (f.fb_id != 0)
            {
                drmModeRmFB(drm.card_fd, f.fb_id);
            }
            if (f.gem_handle != 0)
            {
                drm_gem_close gc{};
                gc.handle = f.gem_handle;
                ioctl(drm.card_fd, DRM_IOCTL_GEM_CLOSE, &gc);
            }
            free_dma_buffer(f);
        }
        cleanup_drm(drm);
        return 1;
    }

    const uint32_t flips = seconds * fps;
    const useconds_t frame_us = static_cast<useconds_t>(1000000U / fps);

    std::cout << "Start page-flip loop: " << flips << " flips, " << fps << " fps\n";

    for (uint32_t i = 1; i <= flips; ++i)
    {
        DmaFrame &f = frames[i % count];
        if (!queue_page_flip_and_wait(drm, f.fb_id))
        {
            std::cerr << "Flip failed at " << i << "\n";
            break;
        }

        std::cout << "flip " << i << " -> fb_id=" << f.fb_id << " dma_fd=" << f.dma_fd << "\n";
        usleep(frame_us);
    }

    for (auto &f : frames)
    {
        if (f.fb_id != 0)
        {
            drmModeRmFB(drm.card_fd, f.fb_id);
        }
        if (f.gem_handle != 0)
        {
            drm_gem_close gc{};
            gc.handle = f.gem_handle;
            ioctl(drm.card_fd, DRM_IOCTL_GEM_CLOSE, &gc);
        }
        free_dma_buffer(f);
    }

    cleanup_drm(drm);
    return 0;
}
