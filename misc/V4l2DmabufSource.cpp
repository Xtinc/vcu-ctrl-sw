// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#include "V4l2DmabufSource.h"

#include <stdexcept>
#include <cstring>
#include <string>
#include <memory>

#ifdef __linux__
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#endif

namespace
{
#ifdef __linux__
inline void xioctl(int fd, unsigned long request, void* arg, const char* what)
{
  if(ioctl(fd, request, arg) < 0)
    throw std::runtime_error(std::string("V4L2 ioctl failed (") + what + "): " + std::strerror(errno));
}
#endif
}

V4l2DmabufSource::V4l2DmabufSource(RealtimeEncoder& enc,
                                   const std::string& devicePath,
                                   uint32_t width,
                                   uint32_t height,
                                   uint32_t pixelFormat,
                                   uint32_t bufferCount)
  : m_enc(enc)
  , m_devicePath(devicePath)
  , m_width(width)
  , m_height(height)
  , m_pixelFormat(pixelFormat)
  , m_bufferCount(bufferCount)
  , m_state(std::make_shared<SharedState>())
{
#ifndef __linux__
  (void)m_enc;
  throw std::runtime_error("V4l2DmabufSource is only supported on Linux");
#else
  openDevice();
  configureFormat();
  requestBuffers();
  exportBuffers();
  queueAllBuffers();

  std::weak_ptr<SharedState> weakState = m_state;
  m_enc.setSourceReleaseCallback([weakState](uint64_t token)
  {
    auto state = weakState.lock();
    if(!state)
      return;
    if(!state->running.load())
      return;

    try
    {
      V4l2DmabufSource::queueBufferByIndex(state, static_cast<uint32_t>(token));
    }
    catch(...)
    {
    }
  });

  streamOn();
#endif
}

V4l2DmabufSource::~V4l2DmabufSource()
{
  try
  {
    stop();
  }
  catch(...)
  {
  }
}

bool V4l2DmabufSource::pushNextFrame()
{
#ifndef __linux__
  return false;
#else
  if(!m_state->running.load())
    return false;

  struct v4l2_buffer buf;
  std::memset(&buf, 0, sizeof(buf));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;

  if(ioctl(m_state->fd, VIDIOC_DQBUF, &buf) < 0)
  {
    if(errno == EAGAIN)
      return true;
    return false;
  }

  if(buf.index >= m_state->buffers.size())
    return false;

  auto& cap = m_state->buffers[buf.index];
  if(cap.fd < 0 || cap.length == 0)
    return false;

  if(!m_enc.submitDmabufFd(cap.fd, cap.length, static_cast<uint64_t>(buf.index)))
  {
    queueBufferByIndex(m_state, buf.index);
    return false;
  }

  return true;
#endif
}

void V4l2DmabufSource::stop()
{
#ifndef __linux__
  return;
#else
  bool wasRunning = m_state->running.exchange(false);

  if(m_state->fd >= 0 && wasRunning)
  {
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    (void)ioctl(m_state->fd, VIDIOC_STREAMOFF, &type);
  }

  m_enc.setSourceReleaseCallback(nullptr);

  for(auto& b : m_state->buffers)
  {
    if(b.fd >= 0)
    {
      close(b.fd);
      b.fd = -1;
    }
  }
  m_state->buffers.clear();

  if(m_state->fd >= 0)
  {
    close(m_state->fd);
    m_state->fd = -1;
  }
#endif
}

void V4l2DmabufSource::openDevice()
{
#ifdef __linux__
  m_state->fd = open(m_devicePath.c_str(), O_RDWR);
  if(m_state->fd < 0)
    throw std::runtime_error("Failed to open V4L2 device: " + m_devicePath + ", error=" + std::strerror(errno));
#endif
}

void V4l2DmabufSource::configureFormat()
{
#ifdef __linux__
  struct v4l2_format fmt;
  std::memset(&fmt, 0, sizeof(fmt));
  fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  fmt.fmt.pix.width = m_width;
  fmt.fmt.pix.height = m_height;
  fmt.fmt.pix.pixelformat = m_pixelFormat;
  fmt.fmt.pix.field = V4L2_FIELD_NONE;
  xioctl(m_state->fd, VIDIOC_S_FMT, &fmt, "VIDIOC_S_FMT");
#endif
}

void V4l2DmabufSource::requestBuffers()
{
#ifdef __linux__
  struct v4l2_requestbuffers req;
  std::memset(&req, 0, sizeof(req));
  req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  req.memory = V4L2_MEMORY_MMAP;
  req.count = m_bufferCount;
  xioctl(m_state->fd, VIDIOC_REQBUFS, &req, "VIDIOC_REQBUFS");

  if(req.count == 0)
    throw std::runtime_error("VIDIOC_REQBUFS returned zero buffers");

  m_state->buffers.resize(req.count);
#endif
}

void V4l2DmabufSource::exportBuffers()
{
#ifdef __linux__
  for(uint32_t i = 0; i < m_state->buffers.size(); ++i)
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
#endif
}

void V4l2DmabufSource::queueAllBuffers()
{
#ifdef __linux__
  for(uint32_t i = 0; i < m_state->buffers.size(); ++i)
    queueBufferByIndex(m_state, i);
#endif
}

void V4l2DmabufSource::streamOn()
{
#ifdef __linux__
  enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  xioctl(m_state->fd, VIDIOC_STREAMON, &type, "VIDIOC_STREAMON");
  m_state->running.store(true);
#endif
}

void V4l2DmabufSource::queueBufferByIndex(const std::shared_ptr<SharedState>& state, uint32_t index)
{
#ifdef __linux__
  if(!state)
    return;

  if(index >= state->buffers.size() || state->fd < 0)
    return;

  std::lock_guard<std::mutex> lock(state->qbufMutex);

  struct v4l2_buffer buf;
  std::memset(&buf, 0, sizeof(buf));
  buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  buf.memory = V4L2_MEMORY_MMAP;
  buf.index = index;

  if(ioctl(state->fd, VIDIOC_QBUF, &buf) < 0)
  {
    if(errno != EBUSY)
      return;
  }
#endif
}
