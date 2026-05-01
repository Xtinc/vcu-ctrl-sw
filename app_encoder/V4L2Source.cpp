#include "V4L2Source.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <cerrno>
#include <cstring>
#include <fcntl.h>
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
    return state == State::IMPORTED || state == State::STOPPED;
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

static bool wait_readable(int fd, int timeout_ms)
{
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN | POLLPRI;

    int ret = -1;
    do
    {
        ret = poll(&pfd, 1, timeout_ms);
    } while (ret < 0 && errno == EINTR);

    if (ret == 0)
    {
        errno = ETIMEDOUT;
        return false;
    }

    if (ret < 0)
    {
        VIDEO_ERROR_PRINT("poll failed before dequeue: %s", std::strerror(errno));
        return false;
    }

    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    {
        VIDEO_ERROR_PRINT("poll reported device error before dequeue: revents=0x%x", pfd.revents);
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

V4L2Source::V4L2Source(const std::string &dev, int req_width, int req_height, TFourCC req_fourcc, int buf_cnt,
                       bool multiple_planes)
    : m_fd(-1), m_buffer_count(buf_cnt),
      m_buf_type(multiple_planes ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE), m_plane_size(0),
      m_state(State::INIT), m_consecutive_timeout_count(0), m_timeout_error_threshold(MAX_TIMEOUT_THRESHOLD)
{
    if (req_width <= 0 || req_height <= 0 || m_buffer_count <= 0)
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

        v4l2_requestbuffers reqbuf{};
        reqbuf.count = m_buffer_count;
        reqbuf.type = m_buf_type;
        reqbuf.memory = V4L2_MEMORY_DMABUF;
        xioctl(m_fd, VIDIOC_REQBUFS, &reqbuf, "request buffers");
        if (reqbuf.count < static_cast<uint32_t>(m_buffer_count))
        {
            throw std::runtime_error("V4L2 device could not allocate enough buffers");
        }
    }
    catch (...)
    {
        if (m_fd >= 0)
        {
            ::close(m_fd);
            m_fd = -1;
        }
        throw;
    }
}

V4L2Source::~V4L2Source()
{
    (void)stop();

    if (m_fd >= 0)
    {
        v4l2_requestbuffers reqbuf{};
        reqbuf.count = 0;
        reqbuf.type = m_buf_type;
        reqbuf.memory = V4L2_MEMORY_DMABUF;
        if (ioctl_retry(m_fd, VIDIOC_REQBUFS, &reqbuf) < 0 && errno != EINVAL)
        {
            VIDEO_ERROR_PRINT("Failed to release V4L2 buffers: %s", std::strerror(errno));
        }
    }

    m_buffers.clear();

    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }

    m_state.store(State::CLOSED);
}

bool V4L2Source::import_fds(const std::vector<int> &fds)
{
    if (m_fd < 0)
    {
        VIDEO_ERROR_PRINT("V4L2 device is not open");
        enter_error_state();
        return false;
    }

    const State state = m_state.load();
    if (state == State::CLOSED || state == State::ERROR)
    {
        VIDEO_ERROR_PRINT("Cannot import FDs in state=%s", state_to_cstr(state));
        return false;
    }

    if (!can_import(state))
    {
        VIDEO_ERROR_PRINT("Cannot import FDs in state=%s", state_to_cstr(state));
        return false;
    }

    if (fds.size() != static_cast<size_t>(m_buffer_count))
    {
        VIDEO_ERROR_PRINT("Number of provided FDs (%zu) does not match buffer count (%d)", fds.size(), m_buffer_count);
        return false;
    }

    m_buffers.resize(m_buffer_count);

    for (size_t i = 0; i < static_cast<size_t>(m_buffer_count); ++i)
    {
        auto &b = m_buffers[i];
        std::memset(&b.buf, 0, sizeof(b.buf));
        std::memset(&b.plane, 0, sizeof(b.plane));
        b.buf.index = i;
        b.buf.type = m_buf_type;
        b.buf.memory = V4L2_MEMORY_DMABUF;
        if (fds[i] < 0)
        {
            VIDEO_ERROR_PRINT("Invalid dma fd at index %zu: %d", i, fds[i]);
            enter_error_state();
            return false;
        }

        if (V4L2_TYPE_IS_MULTIPLANAR(m_buf_type))
        {
            b.buf.length = 1;
            b.buf.m.planes = b.plane;
            b.plane[0].m.fd = fds[i];
            b.plane[0].length = m_plane_size;
        }
        else
        {
            b.buf.length = m_plane_size;
            b.buf.m.fd = fds[i];
        }

        if (!qbuf_idx(i))
        {
            enter_error_state();
            return false;
        }
    }

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

    if (cur_state == State::STOPPED)
    {
        for (int i = 0; i < m_buffer_count; ++i)
        {
            if (!qbuf_idx(i))
            {
                VIDEO_ERROR_PRINT("Failed to requeue buffer idx=%d before STREAMON", i);
                return false;
            }
        }
    }

    int type = m_buf_type;
    auto ret = ioctl_retry(m_fd, VIDIOC_STREAMON, &type);
    if (ret != 0)
    {
        VIDEO_ERROR_PRINT("Failed to start streaming: %s", std::strerror(errno));
        enter_error_state();
        return false;
    }

    stop_requeue_worker();
    m_state.store(State::STREAMING);
    try
    {
        m_requeue_thread = std::thread(&V4L2Source::requeue_worker_loop, this);
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("Failed to start requeue worker: %s", e.what());
        (void)ioctl_retry(m_fd, VIDIOC_STREAMOFF, &type);
        enter_error_state();
        return false;
    }
    m_consecutive_timeout_count = 0;
    return true;
}

bool V4L2Source::stop()
{
    const State state = m_state.load();
    if (!can_stop(state))
    {
        stop_requeue_worker();
        return true;
    }

    if (state == State::INIT)
    {
        stop_requeue_worker();
        return true;
    }

    if (state != State::STREAMING && state != State::ERROR)
    {
        m_state.store(State::STOPPED);
        stop_requeue_worker();
        return true;
    }

    m_state.store(State::STOPPED);
    stop_requeue_worker();

    if (m_fd < 0)
    {
        m_state.store(State::STOPPED);
        return true;
    }

    int type = m_buf_type;
    auto ret = ioctl_retry(m_fd, VIDIOC_STREAMOFF, &type);
    if (ret != 0)
    {
        if (errno == EINVAL)
        {
            // Device may already be in non-streaming state.
            m_state.store(State::STOPPED);
            return true;
        }
        VIDEO_ERROR_PRINT("Failed to stop streaming: %s", std::strerror(errno));
        m_state.store(State::ERROR);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_requeue_mutex);
        m_requeue_pending.clear();
    }
    m_state.store(State::STOPPED);
    return true;
}

bool V4L2Source::queue_idx(unsigned int idx)
{
    if (!can_queue(m_state.load()) || idx >= m_buffers.size())
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_requeue_mutex);
        m_requeue_pending.push_back(idx);
    }

    m_requeue_cv.notify_one();

    return true;
}

bool V4L2Source::dqueue_idx(unsigned int &idx)
{
    if (!can_dequeue(m_state.load()))
    {
        VIDEO_ERROR_PRINT("Cannot dequeue in state=%s", state_to_cstr(m_state.load()));
        return false;
    }

    if (!wait_readable(m_fd, V4L2_DQ_TIMEOUT_MS))
    {
        if (errno == ETIMEDOUT)
        {
            ++m_consecutive_timeout_count;
            if (m_consecutive_timeout_count >= m_timeout_error_threshold)
            {
                VIDEO_ERROR_PRINT("V4L2 dequeue timed out %d times consecutively", m_consecutive_timeout_count);
                enter_error_state();
            }
            return false;
        }
        enter_error_state();
        return false;
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
            ++m_consecutive_timeout_count;
            if (m_consecutive_timeout_count >= m_timeout_error_threshold)
            {
                VIDEO_ERROR_PRINT("V4L2 dequeue EAGAIN %d times consecutively", m_consecutive_timeout_count);
                enter_error_state();
            }
            return false;
        }

        if (errno == EPIPE || errno == EINVAL)
        {
            enter_error_state();
        }

        if (errno != EAGAIN)
        {
            VIDEO_ERROR_PRINT("Failed to dequeue buffer: %s", std::strerror(errno));
        }
        enter_error_state();
        return false;
    }

    m_consecutive_timeout_count = 0;

    if (buf.index >= static_cast<uint32_t>(m_buffer_count))
    {
        VIDEO_ERROR_PRINT("Driver returned invalid dequeued index: %u", buf.index);
        enter_error_state();
        return false;
    }

    idx = buf.index;
    return true;
}

bool V4L2Source::qbuf_idx(unsigned int idx)
{
    if (idx >= m_buffers.size())
    {
        return false;
    }

    auto &b = m_buffers[idx];
    if (ioctl_retry(m_fd, VIDIOC_QBUF, &b.buf) < 0)
    {
        VIDEO_ERROR_PRINT("Failed to queue buffer idx=%d: %s", idx, std::strerror(errno));
        enter_error_state();
        return false;
    }

    return true;
}

void V4L2Source::stop_requeue_worker()
{
    m_requeue_cv.notify_all();

    if (m_requeue_thread.joinable())
    {
        m_requeue_thread.join();
    }

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
    m_requeue_cv.notify_all();
}