// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include "RealtimeEncoder.h"

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include <atomic>

/**
 * @brief 基于 v4l2 capture + dmabuf 导出 + RealtimeEncoder::submitDmabufFd 的零拷贝输入源。
 *
 * 仅支持单平面采集队列（V4L2_BUF_TYPE_VIDEO_CAPTURE）。
 */
class V4l2DmabufSource
{
public:
  V4l2DmabufSource(RealtimeEncoder& enc,
                   const std::string& devicePath,
                   uint32_t width,
                   uint32_t height,
                   uint32_t pixelFormat,
                   uint32_t bufferCount = 4);

  ~V4l2DmabufSource();

  V4l2DmabufSource(const V4l2DmabufSource&) = delete;
  V4l2DmabufSource& operator=(const V4l2DmabufSource&) = delete;

  /**
   * @brief 从 v4l2 取一帧并提交编码器。
   * @return true 成功提交；false 发生错误。
   */
  bool pushNextFrame();

  /**
   * @brief 停止采集并释放资源（可重复调用）。
   */
  void stop();

private:
  struct CaptureBuffer
  {
    int fd = -1;
    size_t length = 0;
  };

  struct SharedState
  {
    int fd = -1;
    std::vector<CaptureBuffer> buffers;
    std::mutex qbufMutex;
    std::atomic<bool> running{false};
  };

  void openDevice();
  void configureFormat();
  void requestBuffers();
  void exportBuffers();
  void queueAllBuffers();
  void streamOn();
  static void queueBufferByIndex(const std::shared_ptr<SharedState>& state, uint32_t index);

  RealtimeEncoder& m_enc;
  std::string m_devicePath;
  uint32_t m_width = 0;
  uint32_t m_height = 0;
  uint32_t m_pixelFormat = 0;
  uint32_t m_bufferCount = 0;
  std::shared_ptr<SharedState> m_state;
};
