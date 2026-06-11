#include "V4L2Source.h"
#include "XilinxSyncIP.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/v4l2-subdev.h>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

static constexpr auto SUPPORTED_FOURCC_NV12 = FOURCC(NV12);
static constexpr auto SUPPORTED_FOURCC_NV16 = FOURCC(NV16);
static constexpr int V4L2_DQ_TIMEOUT_MS = 1000;
static constexpr int MAX_TIMEOUT_THRESHOLD = 30;

const char *V4L2Source::state_to_cstr(State state)
{
    switch (state)
    {
    case State::INIT:
        return "INIT";
    case State::IMPORTED:
        return "IMPORTED";
    case State::STREAMING:
        return "STREAMING";
    case State::STOPPED:
        return "STOPPED";
    case State::ERROR:
        return "ERROR";
    case State::CLOSED:
        return "CLOSED";
    }
    return "UNKNOWN";
}

bool V4L2Source::can_import(State state)
{
    return state == State::INIT || state == State::STOPPED;
}

bool V4L2Source::can_start(State state)
{
    return state == State::IMPORTED;
}

bool V4L2Source::can_stop(State state)
{
    return state != State::CLOSED;
}

bool V4L2Source::can_queue(State state)
{
    return state == State::STREAMING;
}

bool V4L2Source::can_dequeue(State state)
{
    return state == State::STREAMING;
}

static void xioctl(int fd, unsigned long request, void *arg, const char *what)
{
    int ret = -1;
    do
    {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);

    if (ret < 0)
    {
        throw std::runtime_error(std::string("V4L2 ioctl failed (") + what + "): " + std::strerror(errno));
    }
}

static int ioctl_retry(int fd, unsigned long request, void *arg)
{
    int ret = -1;
    do
    {
        ret = ioctl(fd, request, arg);
    } while (ret < 0 && errno == EINTR);
    return ret;
}

struct PollRevents
{
    int main_fd;
    int sub_fd;
};

static PollRevents wait_readable(int fd, int sub_fd, int timeout_ms)
{
    int nfds = 1;
    pollfd pfds[2]{};
    pfds[0].fd = fd;
    pfds[0].events = POLLIN | POLLPRI;
    if (sub_fd >= 0)
    {
        pfds[1].fd = sub_fd;
        pfds[1].events = POLLPRI;
        nfds = 2;
    }

    int ret = -1;
    do
    {
        ret = poll(pfds, nfds, timeout_ms);
    } while (ret < 0 && errno == EINTR);

    if (ret == 0)
        return {0, 0};

    if (ret < 0)
    {
        VIDEO_ERROR_PRINT("poll failed: %s", std::strerror(errno));
        return {-1, 0};
    }

    return {(int)pfds[0].revents, (nfds > 1) ? (int)pfds[1].revents : 0};
}

static bool set_xilinx_low_latency_mode(int fd, bool enable)
{
    v4l2_control control{};
    control.id = V4L2_CID_XILINX_LOW_LATENCY;
    control.value = enable ? XVIP_LOW_LATENCY_ENABLE : XVIP_LOW_LATENCY_DISABLE;

    if (ioctl(fd, VIDIOC_S_CTRL, &control) == -1)
    {
        VIDEO_ERROR_PRINT("Failed to %s Xilinx low-latency mode: %s", enable ? "enable" : "disable",
                          std::strerror(errno));
        return false;
    }
    return true;
}

static bool release_v4l2_buffers(int fd, int buf_type)
{
    if (fd < 0)
    {
        return true;
    }

    v4l2_requestbuffers reqbuf{};
    reqbuf.count = 0;
    reqbuf.type = buf_type;
    reqbuf.memory = V4L2_MEMORY_DMABUF;
    if (ioctl_retry(fd, VIDIOC_REQBUFS, &reqbuf) < 0 && errno != EINVAL)
    {
        VIDEO_ERROR_PRINT("Failed to release V4L2 buffers: %s", std::strerror(errno));
        return false;
    }

    return true;
}

static void dump_v4l2_format(const v4l2_format &fmt)
{
    if (V4L2_TYPE_IS_MULTIPLANAR(fmt.type))
    {
        const v4l2_pix_format_mplane *pix = &fmt.fmt.pix_mp;
        VIDEO_INFO_PRINT("MultiPlane:%ux%u format=%.4s bpl=%u", pix->width, pix->height, (char *)&pix->pixelformat,
                         pix->plane_fmt[0].bytesperline);
    }
    else
    {
        const v4l2_pix_format *pix = &fmt.fmt.pix;
        VIDEO_INFO_PRINT("SinglePlane:%ux%u format=%.4s bpl=%u\n", pix->width, pix->height, (char *)&pix->pixelformat,
                         pix->bytesperline);
    }
}

V4L2Source::V4L2Source(const std::string &dev, const std::string &sub_dev, int req_width, int req_height,
                       TFourCC req_fourcc, size_t buf_cnt, bool multiple_planes, const std::string &sync_dev_path)
    : m_fd(-1), m_sub_fd(-1), m_buffer_cnt(buf_cnt),
      m_buf_type(multiple_planes ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE), m_plane_size(0),
      m_state(State::INIT), m_consecutive_timeout_count(0), m_buffers_queued(false), m_sync_ip(nullptr)
{
    if (req_width <= 0 || req_height <= 0 || buf_cnt == 0)
    {
        throw std::invalid_argument("Invalid V4L2 source geometry/buffer count");
    }

    if (req_fourcc != SUPPORTED_FOURCC_NV12 && req_fourcc != SUPPORTED_FOURCC_NV16)
    {
        throw std::invalid_argument("Unsupported pixel format (FOURCC): " + std::to_string(req_fourcc));
    }

    const int pix_fmt = (req_fourcc == SUPPORTED_FOURCC_NV12) ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_NV16;

    m_fd = ::open(dev.c_str(), O_RDWR | O_NONBLOCK);
    if (m_fd < 0)
    {
        throw std::runtime_error("Failed to open V4L2 device: " + dev + " - " + std::strerror(errno));
    }

    m_sub_fd = ::open(sub_dev.c_str(), O_RDWR | O_NONBLOCK);
    if (m_sub_fd < 0)
    {
        ::close(m_fd);
        m_fd = -1;
        throw std::runtime_error("Failed to open V4L2 subdevice: " + sub_dev + " - " + std::strerror(errno));
    }

    if (!sync_dev_path.empty() && set_xilinx_low_latency_mode(m_fd, true))
    {
        m_sync_ip = std::make_unique<XilinxSyncIP>(sync_dev_path);
        if (!m_sync_ip->init())
        {
            VIDEO_ERROR_PRINT("Xilinx sync IP init failed, fallback to standard mode");
            m_sync_ip.reset();
            (void)set_xilinx_low_latency_mode(m_fd, false);
        }
        else
        {
            VIDEO_INFO_PRINT("Xilinx hardware synchronization enabled [%s]", sync_dev_path.c_str());
        }
    }

    try
    {
        v4l2_format fmt{};
        fmt.type = m_buf_type;
        if (V4L2_TYPE_IS_MULTIPLANAR(m_buf_type))
        {
            fmt.fmt.pix_mp.width = req_width;
            fmt.fmt.pix_mp.height = req_height;
            fmt.fmt.pix_mp.pixelformat = pix_fmt;
            fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
            fmt.fmt.pix_mp.num_planes = 1;
        }
        else
        {
            fmt.fmt.pix.width = req_width;
            fmt.fmt.pix.height = req_height;
            fmt.fmt.pix.pixelformat = pix_fmt;
            fmt.fmt.pix.field = V4L2_FIELD_NONE;
        }
        xioctl(m_fd, VIDIOC_S_FMT, &fmt, "set format");

        std::memset(&fmt, 0, sizeof(fmt));
        fmt.type = m_buf_type;
        xioctl(m_fd, VIDIOC_G_FMT, &fmt, "get format");

        bool format_ok = false;
        if (V4L2_TYPE_IS_MULTIPLANAR(m_buf_type))
        {
            format_ok = (fmt.fmt.pix_mp.width == static_cast<uint32_t>(req_width) &&
                         fmt.fmt.pix_mp.height == static_cast<uint32_t>(req_height) &&
                         fmt.fmt.pix_mp.pixelformat == static_cast<uint32_t>(pix_fmt));
            m_plane_size = fmt.fmt.pix_mp.plane_fmt[0].sizeimage;
        }
        else
        {
            format_ok = (fmt.fmt.pix.width == static_cast<uint32_t>(req_width) &&
                         fmt.fmt.pix.height == static_cast<uint32_t>(req_height) &&
                         fmt.fmt.pix.pixelformat == static_cast<uint32_t>(pix_fmt));
            m_plane_size = fmt.fmt.pix.sizeimage;
        }

        if (!format_ok)
        {
            throw std::runtime_error("V4L2 device did not accept requested format");
        }

        if (m_plane_size == 0)
        {
            throw std::runtime_error("V4L2 returned invalid sizeimage=0");
        }

        dump_v4l2_format(fmt);
    }
    catch (...)
    {
        ::close(m_sub_fd);
        m_sub_fd = -1;
        ::close(m_fd);
        m_fd = -1;
        throw;
    }
}

V4L2Source::~V4L2Source()
{
    (void)stop();
    m_buffers.clear();
    ::close(m_sub_fd);
    m_sub_fd = -1;
    ::close(m_fd);
    m_fd = -1;
    m_state.store(State::CLOSED);
}

bool V4L2Source::import_fds(DMAFdArray &&fds)
{
    if (m_fd < 0)
    {
        VIDEO_ERROR_PRINT("V4L2 device is not open");
        enter_error_state();
        return false;
    }

    const State state = m_state.load();
    if (state == State::CLOSED)
    {
        VIDEO_ERROR_PRINT("Cannot import FDs in state=%s", state_to_cstr(state));
        return false;
    }

    if (!can_import(state))
    {
        VIDEO_ERROR_PRINT("Cannot import FDs in state=%s", state_to_cstr(state));
        return false;
    }

    if (fds.size() != m_buffer_cnt)
    {
        VIDEO_ERROR_PRINT("Number of provided buffers (%zu) does not match buffer count (%zu)", fds.size(),
                          m_buffer_cnt);
        return false;
    }

    v4l2_requestbuffers reqbuf{};
    reqbuf.count = m_buffer_cnt;
    reqbuf.type = m_buf_type;
    reqbuf.memory = V4L2_MEMORY_DMABUF;
    if (ioctl_retry(m_fd, VIDIOC_REQBUFS, &reqbuf) < 0)
    {
        VIDEO_ERROR_PRINT("Failed to request V4L2 buffers: %s", std::strerror(errno));
        enter_error_state();
        return false;
    }
    if (reqbuf.count < static_cast<uint32_t>(m_buffer_cnt))
    {
        VIDEO_ERROR_PRINT("V4L2 device could not allocate enough buffers (%u < %zu)", reqbuf.count, m_buffer_cnt);
        enter_error_state();
        return false;
    }

    m_buffers.resize(m_buffer_cnt);

    for (size_t i = 0; i < m_buffer_cnt; ++i)
    {
        auto &b = m_buffers[i];
        std::memset(&b.buf, 0, sizeof(b.buf));
        std::memset(&b.plane, 0, sizeof(b.plane));
        b.buf.index = i;
        b.buf.type = m_buf_type;
        b.buf.memory = V4L2_MEMORY_DMABUF;
        b.sync_desc = std::move(fds[i]);
        if (b.sync_desc.dma_fd < 0)
        {
            VIDEO_ERROR_PRINT("Invalid dma fd at index %zu: %d", i, b.sync_desc.dma_fd);
            enter_error_state();
            return false;
        }

        if (V4L2_TYPE_IS_MULTIPLANAR(m_buf_type))
        {
            b.buf.length = 1;
            b.buf.m.planes = b.plane;
            b.plane[0].m.fd = b.sync_desc.dma_fd;
            b.plane[0].length = m_plane_size;
        }
        else
        {
            b.buf.length = m_plane_size;
            b.buf.m.fd = b.sync_desc.dma_fd;
        }

        if (!qbuf_idx(i))
        {
            VIDEO_ERROR_PRINT("qbuf_idx(%zu) failed during import_fds, releasing V4L2 buffers", i);
            // Clean up V4L2 resources since some buffers were already queued
            (void)release_v4l2_buffers(m_fd, m_buf_type);
            m_buffers.clear();
            enter_error_state();
            return false;
        }
    }

    m_buffers_queued.store(true);
    m_state.store(State::IMPORTED);
    return true;
}

bool V4L2Source::start()
{
    const State cur_state = m_state.load();
    if (cur_state == State::STREAMING)
    {
        return true;
    }

    if (m_fd < 0)
    {
        VIDEO_ERROR_PRINT("Cannot start V4L2 stream: device is closed");
        enter_error_state();
        return false;
    }

    if (m_buffers.empty())
    {
        VIDEO_ERROR_PRINT("Cannot start V4L2 stream: no imported DMA buffers");
        enter_error_state();
        return false;
    }

    if (!can_start(cur_state))
    {
        VIDEO_ERROR_PRINT("Cannot start V4L2 stream in state=%s", state_to_cstr(cur_state));
        return false;
    }

    int type = m_buf_type;
    auto ret = ioctl_retry(m_fd, VIDIOC_STREAMON, &type);
    if (ret != 0)
    {
        VIDEO_ERROR_PRINT("Failed to start streaming: %s", std::strerror(errno));
        enter_error_state();
        return false;
    }

    if (m_sync_ip && !m_sync_ip->start())
    {
        VIDEO_ERROR_PRINT("Failed to start Xilinx sync IP");
        goto error_cleanup;
    }

    if (m_sync_ip)
    {
        v4l2_control control{};
        control.id = V4L2_CID_XILINX_LOW_LATENCY;
        control.value = XVIP_START_DMA;

        if (ioctl(m_fd, VIDIOC_S_CTRL, &control) == -1)
        {
            VIDEO_ERROR_PRINT("Failed to start DMA: %s", std::strerror(errno));
            goto error_cleanup;
        }
        VIDEO_INFO_PRINT("Start Xilinx DMA synchronization");
    }

    {
        v4l2_event_subscription sub{};
        sub.type = V4L2_EVENT_SOURCE_CHANGE;
        int sub_target = (m_sub_fd >= 0) ? m_sub_fd : m_fd;
        if (ioctl_retry(sub_target, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
            VIDEO_INFO_PRINT("VIDIOC_SUBSCRIBE_EVENT not supported, source-change detection disabled: %s",
                             std::strerror(errno));
    }

    stop_requeue_worker();
    try
    {
        m_requeue_thread = std::thread(&V4L2Source::requeue_worker_loop, this);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("Failed to start requeue worker: %s", e.what());
        goto error_cleanup;
    }
    
    // Set STREAMING state only after all resources (including worker thread) are ready
    m_state.store(State::STREAMING);
    m_consecutive_timeout_count = 0;
    return true;

error_cleanup:
    (void)ioctl_retry(m_fd, VIDIOC_STREAMOFF, &type);
    enter_error_state();
    return false;
}

bool V4L2Source::stop()
{
    const State state = m_state.load();
    if (!can_stop(state))
    {
        stop_requeue_worker();
        return true;
    }

    m_state.store(State::STOPPED);
    stop_requeue_worker();

    if (m_fd < 0)
    {
        return true;
    }

    if (state == State::STREAMING || state == State::ERROR)
    {
        int type = m_buf_type;
        auto ret = ioctl_retry(m_fd, VIDIOC_STREAMOFF, &type);
        if (ret != 0 && errno != EINVAL)
        {
            VIDEO_ERROR_PRINT("Failed to stop streaming: %s", std::strerror(errno));
            m_state.store(State::ERROR);
            return false;
        }
    }

    if (!release_v4l2_buffers(m_fd, m_buf_type))
    {
        m_state.store(State::ERROR);
        return false;
    }

    m_buffers_queued.store(false);
    m_buffers.clear();

    {
        std::lock_guard<std::mutex> lock(m_requeue_mutex);
        m_requeue_pending.clear();
    }

    return true;
}

bool V4L2Source::queue(AL_TBuffer const *buffer)
{
    if (!buffer)
    {
        VIDEO_ERROR_PRINT("Cannot queue null buffer");
        return false;
    }

    if (!can_queue(m_state.load()))
    {
        VIDEO_ERROR_PRINT("Cannot queue buffer in state=%s", state_to_cstr(m_state.load()));
        return false;
    }

    auto iter = std::find_if(m_buffers.cbegin(), m_buffers.cend(),
                             [buffer](const v4l2_buffer_info &b) { return b.sync_desc.buffer == buffer; });
    if (iter == m_buffers.cend())
    {
        VIDEO_ERROR_PRINT("Provided buffer pointer does not match any imported buffer");
        return false;
    }

    auto idx = std::distance(m_buffers.cbegin(), iter);
    {
        std::lock_guard<std::mutex> lock(m_requeue_mutex);
        m_requeue_pending.push_back(idx);
    }

    m_requeue_cv.notify_one();
    return true;
}

DQStatus V4L2Source::handle_pollpri_event(int revents)
{
    if (!(revents & POLLPRI))
    {
        return DQStatus::OK;
    }

    bool source_changed = false;
    v4l2_event event{};
    while (ioctl_retry(m_sub_fd, VIDIOC_DQEVENT, &event) == 0)
    {
        if (event.type == V4L2_EVENT_SOURCE_CHANGE)
        {
            // Received source change event - could be resolution change, signal loss, etc.
            // Don't check changes field (not reliable across all drivers/kernels)
            // Upper layer (EncMgr) will probe device to determine actual change
            source_changed = true;
        }
    }

    if (source_changed)
    {
        VIDEO_INFO_PRINT("V4L2 source change event received, will probe device status");
        return DQStatus::SourceChanged;
    }

    return DQStatus::OK;
}

DQResult V4L2Source::handle_timeout(const char *reason)
{
    ++m_consecutive_timeout_count;
    if (m_consecutive_timeout_count >= MAX_TIMEOUT_THRESHOLD)
    {
        VIDEO_ERROR_PRINT("V4L2 dequeue %s %d times consecutively", reason, m_consecutive_timeout_count);
        enter_error_state();
        return {DQStatus::Error, nullptr};
    }
    return {DQStatus::Timeout, nullptr};
}

DQResult V4L2Source::dqueue()
{
    const State state = m_state.load();
    if (!can_dequeue(state))
    {
        VIDEO_ERROR_PRINT("Cannot dequeue in state=%s", state_to_cstr(state));
        return {DQStatus::Error, nullptr};
    }

    const PollRevents poll_result = wait_readable(m_fd, m_sub_fd, V4L2_DQ_TIMEOUT_MS);

    if (poll_result.main_fd == 0 && poll_result.sub_fd == 0)
    {
        return handle_timeout("timed out");
    }

    if (poll_result.main_fd < 0)
    {
        enter_error_state();
        return {DQStatus::Error, nullptr};
    }

    if (poll_result.main_fd & (POLLERR | POLLHUP | POLLNVAL))
    {
        VIDEO_ERROR_PRINT("poll reported device error: revents=0x%x", poll_result.main_fd);
        enter_error_state();
        return {DQStatus::Error, nullptr};
    }

    const DQStatus event_status = handle_pollpri_event(poll_result.sub_fd);
    if (event_status == DQStatus::SourceChanged)
    {
        return {event_status, nullptr};
    }

    if (!(poll_result.main_fd & POLLIN))
    {
        return handle_timeout("no frame ready");
    }

    v4l2_buffer buf{};
    v4l2_plane plane[1]{};
    buf.type = m_buf_type;
    buf.memory = V4L2_MEMORY_DMABUF;
    if (V4L2_TYPE_IS_MULTIPLANAR(m_buf_type))
    {
        buf.length = 1;
        buf.m.planes = plane;
    }

    if (ioctl_retry(m_fd, VIDIOC_DQBUF, &buf) < 0)
    {
        if (errno == EAGAIN)
        {
            return handle_timeout("EAGAIN");
        }

        VIDEO_ERROR_PRINT("Failed to dequeue buffer: %s", std::strerror(errno));
        enter_error_state();
        return {DQStatus::Error, nullptr};
    }

    m_consecutive_timeout_count = 0;

    if (buf.index >= m_buffer_cnt)
    {
        VIDEO_ERROR_PRINT("Driver returned invalid dequeued index: %u", buf.index);
        enter_error_state();
        return {DQStatus::Error, nullptr};
    }

    return {DQStatus::OK, m_buffers[buf.index].sync_desc.buffer};
}

bool V4L2Source::probe_subdev_format(const std::string &subdev, int pad, int &width, int &height)
{
    width = 0;
    height = 0;

    if (subdev.empty())
    {
        VIDEO_ERROR_PRINT("probe_subdev_format: subdev path is empty");
        return false;
    }

    const int fd = ::open(subdev.c_str(), O_RDWR | O_NONBLOCK);
    if (fd < 0)
    {
        VIDEO_ERROR_PRINT("probe_subdev_format: cannot open %s: %s", subdev.c_str(), std::strerror(errno));
        return false;
    }

    v4l2_subdev_format sub_fmt{};
    sub_fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    sub_fmt.pad = pad;

    const bool ok = (ioctl_retry(fd, VIDIOC_SUBDEV_G_FMT, &sub_fmt) == 0);
    ::close(fd);

    if (!ok)
    {
        VIDEO_INFO_PRINT("probe_subdev_format: no active source on %s (pad %d): %s", subdev.c_str(), pad,
                         std::strerror(errno));
        return false;
    }

    width = static_cast<int>(sub_fmt.format.width);
    height = static_cast<int>(sub_fmt.format.height);

    if (width <= 0 || height <= 0)
    {
        VIDEO_ERROR_PRINT("probe_subdev_format: invalid resolution %dx%d from %s", width, height, subdev.c_str());
        return false;
    }

    return true;
}

bool V4L2Source::qbuf_idx(unsigned int idx)
{
    if (idx >= m_buffers.size())
    {
        return false;
    }

    auto &b = m_buffers[idx];

    if (m_sync_ip)
    {
        if (!m_sync_ip->add_buffer(b.sync_desc))
        {
            VIDEO_ERROR_PRINT("Failed to add sync metadata for idx=%u, continue with QBUF", idx);
        }
    }

    if (ioctl_retry(m_fd, VIDIOC_QBUF, &b.buf) < 0)
    {
        VIDEO_ERROR_PRINT("Failed to queue buffer idx=%d: %s", idx, std::strerror(errno));
        return false;
    }

    return true;
}

void V4L2Source::stop_requeue_worker()
{
    // Notify worker thread to exit (predicate will fail when state != STREAMING)
    m_requeue_cv.notify_all();

    // Wait for worker thread to complete
    // IMPORTANT: Must join before acquiring m_requeue_mutex to avoid deadlock
    // (worker thread may be holding the mutex when checking the exit condition)
    if (m_requeue_thread.joinable())
    {
        m_requeue_thread.join();
    }

    // Clear pending queue after thread has exited
    std::lock_guard<std::mutex> lock(m_requeue_mutex);
    m_requeue_pending.clear();
}

void V4L2Source::requeue_worker_loop()
{
    while (true)
    {
        unsigned int idx = 0xffffffff;
        {
            std::unique_lock<std::mutex> lock(m_requeue_mutex);
            m_requeue_cv.wait(lock, [this] { return !can_queue(m_state.load()) || !m_requeue_pending.empty(); });

            if (!can_queue(m_state.load()))
            {
                break;
            }

            idx = m_requeue_pending.front();
            m_requeue_pending.pop_front();
        }

        if (!qbuf_idx(idx))
        {
            enter_error_state();
            break;
        }
    }
}

void V4L2Source::enter_error_state()
{
    m_state.store(State::ERROR);
    m_buffers_queued.store(false);
    m_requeue_cv.notify_all();
    (void)stop();
}
