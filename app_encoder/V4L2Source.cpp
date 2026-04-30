#include "V4L2Source.h"

extern "C"
{
#include "lib_rtos/message.h"
}

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <unistd.h>

static constexpr auto SUPPORTED_FOURCC_NV12 = FOURCC(NV12);
static constexpr auto SUPPORTED_FOURCC_NV16 = FOURCC(NV16);

static void xioctl(int fd, unsigned long request, void *arg, const char *what)
{
    if (ioctl(fd, request, arg) < 0)
    {
        throw std::runtime_error(std::string("V4L2 ioctl failed (") + what + "): " + std::strerror(errno));
    }
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
    : m_fd(-1), m_dev(dev), m_width(req_width), m_height(req_height), m_buffer_count(buf_cnt),
      m_buf_type(multiple_planes ? V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE : V4L2_BUF_TYPE_VIDEO_CAPTURE),
      m_pix_fmt(V4L2_PIX_FMT_NV12), m_streaming(false)
{
    if (req_fourcc != SUPPORTED_FOURCC_NV12 && req_fourcc != SUPPORTED_FOURCC_NV16)
    {
        throw std::invalid_argument("Unsupported pixel format (FOURCC): " + std::to_string(req_fourcc));
    }

    m_pix_fmt = (req_fourcc == SUPPORTED_FOURCC_NV12) ? V4L2_PIX_FMT_NV12 : V4L2_PIX_FMT_NV16;

    m_fd = ::open(m_dev.c_str(), O_RDWR);
    if (m_fd < 0)
    {
        throw std::runtime_error("Failed to open V4L2 device: " + m_dev + " - " + std::strerror(errno));
    }

    try
    {
        v4l2_format fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        fmt.fmt.pix_mp.width = m_width;
        fmt.fmt.pix_mp.height = m_height;
        fmt.fmt.pix_mp.pixelformat = m_pix_fmt;
        fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
        xioctl(m_fd, VIDIOC_S_FMT, &fmt, "set format");

        std::memset(&fmt, 0, sizeof(fmt));
        fmt.type = m_buf_type;
        xioctl(m_fd, VIDIOC_G_FMT, &fmt, "get format");
        if (fmt.fmt.pix_mp.width != static_cast<uint32_t>(m_width) ||
            fmt.fmt.pix_mp.height != static_cast<uint32_t>(m_height) ||
            fmt.fmt.pix_mp.pixelformat != (unsigned int)(m_pix_fmt))
        {
            throw std::runtime_error("V4L2 device did not accept requested format");
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
    if (m_fd >= 0)
    {
        ::close(m_fd);
        m_fd = -1;
    }
}

bool V4L2Source::import_fds(const std::vector<int> &fds)
{
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
        b.buf.length = 1;
        b.buf.m.planes = b.plane;
        b.plane[0].m.fd = fds[i];

        if (ioctl(m_fd, VIDIOC_QBUF, &b.buf) < 0)
        {
            VIDEO_ERROR_PRINT("Failed to queue buffer: %s", std::strerror(errno));
            return false;
        }
    }

    return true;
}

bool V4L2Source::start()
{
    if (m_streaming)
    {
        return true;
    }

    int type = m_buf_type;
    auto ret = ioctl(m_fd, VIDIOC_STREAMON, &type);
    if (ret != 0)
    {
        VIDEO_ERROR_PRINT("Failed to start streaming: %s", std::strerror(errno));
        return false;
    }
    m_streaming = true;
    return true;
}

bool V4L2Source::stop()
{
    if (!m_streaming)
    {
        return true;
    }

    int type = m_buf_type;
    auto ret = ioctl(m_fd, VIDIOC_STREAMOFF, &type);
    if (ret != 0)
    {
        VIDEO_ERROR_PRINT("Failed to stop streaming: %s", std::strerror(errno));
        return false;
    }
    m_streaming = false;
    return true;
}

bool V4L2Source::queue_idx(int idx)
{
    if (!m_streaming || idx < 0 || static_cast<size_t>(idx) >= m_buffers.size())
    {
        return false;
    }

    auto &b = m_buffers[idx];
    if (ioctl(m_fd, VIDIOC_QBUF, &b.buf) < 0)
    {
        VIDEO_ERROR_PRINT("Failed to queue buffer: %s", std::strerror(errno));
        return false;
    }

    return true;
}

int V4L2Source::dequeue_idx()
{
    if (!m_streaming)
    {
        return -1;
    }

    v4l2_buffer buf{};
    v4l2_plane plane[1]{};
    buf.type = m_buf_type;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.length = 1;
    buf.m.planes = plane;

    if (ioctl(m_fd, VIDIOC_DQBUF, &buf) < 0)
    {
        if (errno != EAGAIN)
        {
            VIDEO_ERROR_PRINT("Failed to dequeue buffer: %s", std::strerror(errno));
        }
        return -1;
    }

    return buf.index;
}