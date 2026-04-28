#include "V4L2Source.h"

#if LINUX_OS_ENVIRONMENT

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

static void xioctl(int fd, unsigned long request, void *arg, const char *what)
{
    if (ioctl(fd, request, arg) < 0)
    {
        throw std::runtime_error(std::string("V4L2 ioctl failed (") + what + "): " + std::strerror(errno));
    }
}

V4L2Source::V4L2Source(const std::string &devicePath, uint32_t width, uint32_t height, uint32_t pixelFormat,
                       uint32_t bufferCount)
    : m_dev_path(devicePath), m_width(width), m_height(height), m_pix_fmt(pixelFormat), m_buffer_count(bufferCount),
      m_state(std::make_shared<SharedState>())
{
    open_device();
    config_format();
    request_buffers();
    export_buffers();
    queue_all_buffers();
}

V4L2Source::~V4L2Source()
{
    try
    {
        stop();
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("Exception in V4L2Source destructor: %s", e.what());
    }
}

bool V4L2Source::start()
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    bool ok = (ioctl(m_state->fd, VIDIOC_STREAMON, &type) == 0);
    m_state->running.store(ok);
    return ok;
}

void V4L2Source::stop()
{
    bool was_running = m_state->running.exchange(false);

    if (m_state->fd >= 0 && was_running)
    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        (void)ioctl(m_state->fd, VIDIOC_STREAMOFF, &type);
    }

    {
        std::lock_guard<std::mutex> lock(m_state->qbufMutex);
        for (auto &b : m_state->buffers)
        {
            if (b.fd >= 0)
            {
                ::close(b.fd);
                b.fd = -1;
            }
        }
        m_state->buffers.clear();
    }

    if (m_state->fd >= 0)
    {
        ::close(m_state->fd);
        m_state->fd = -1;
    }
}

bool V4L2Source::read_frame(int &fd, size_t &length)
{
    if (!m_state->running.load())
    {
        return false;
    }

    struct v4l2_buffer buf;
    std::memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    std::lock_guard<std::mutex> lock(m_state->qbufMutex);
    if (ioctl(m_state->fd, VIDIOC_DQBUF, &buf) < 0)
    {
        return false;
    }

    if (buf.index >= m_state->buffers.size())
    {
        return false;
    }

    auto &cap = m_state->buffers[buf.index];
    if (cap.fd < 0 || cap.length == 0)
    {
        return false;
    }

    fd = cap.fd;
    length = cap.length;
    return true;
}

void V4L2Source::open_device()
{
    m_state->fd = ::open(m_dev_path.c_str(), O_RDWR);
    if (m_state->fd < 0)
    {
        throw std::runtime_error("Failed to open V4L2 device: " + m_dev_path + " - " + std::strerror(errno));
    }
}

void V4L2Source::config_format()
{
    struct v4l2_format fmt;
    std::memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = m_width;
    fmt.fmt.pix.height = m_height;
    fmt.fmt.pix.pixelformat = m_pix_fmt;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;

    xioctl(m_state->fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT");
}

void V4L2Source::request_buffers()
{
    struct v4l2_requestbuffers req;
    std::memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    req.count = m_buffer_count;
    xioctl(m_state->fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS");

    if (req.count == 0)
    {
        throw std::runtime_error("VIDIOC_REQBUFS returned zero buffers");
    }

    m_state->buffers.resize(req.count);
}

void V4L2Source::export_buffers()
{
    for (uint32_t i = 0; i < m_state->buffers.size(); ++i)
    {
        struct v4l2_buffer buf;
        std::memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        xioctl(m_state->fd, VIDIOC_QUERYBUF, &buf, "VIDIOC_QUERYBUF");

        struct v4l2_exportbuffer exp;
        std::memset(&exp, 0, sizeof(exp));
        exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        exp.index = i;
        exp.flags = O_CLOEXEC;
        xioctl(m_state->fd, VIDIOC_EXPBUF, &exp, "VIDIOC_EXPBUF");

        m_state->buffers[i].fd = exp.fd;
        m_state->buffers[i].length = buf.length;
    }
}

void V4L2Source::queue_all_buffers()
{
    for (size_t i = 0; i < m_state->buffers.size(); i++)
    {
        queue_buffer_by_index(m_state, i);
    }
}

void V4L2Source::queue_buffer_by_index(const std::shared_ptr<SharedState> &state, uint32_t index)
{
    if (!state)
    {
        return;
    }

    if (index >= state->buffers.size() || state->fd < 0)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(state->qbufMutex);

    struct v4l2_buffer buf;
    std::memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = index;

    if (ioctl(state->fd, VIDIOC_QBUF, &buf) < 0)
    {
        if (errno != EBUSY)
        {
            return;
        }
    }
}

#endif