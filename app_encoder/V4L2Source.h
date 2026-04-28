#ifndef V4L2_SOURCE_H
#define V4L2_SOURCE_H

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C"
{
#include "lib_rtos/lib_rtos.h"
#include "lib_rtos/message.h"
}

#if LINUX_OS_ENVIRONMENT

class V4L2Source
{
  public:
    V4L2Source(const std::string &devicePath, uint32_t width, uint32_t height, uint32_t pixelFormat,
               uint32_t bufferCount = 5);
    ~V4L2Source();

    V4L2Source(const V4L2Source &) = delete;
    V4L2Source &operator=(const V4L2Source &) = delete;

    bool start();
    void stop();
    bool read_frame(int &fd, size_t &length);
    void queue_buffer(int fd)
    {
        std::lock_guard<std::mutex> lock(m_state->qbufMutex);
        for (size_t i = 0; i < m_state->buffers.size(); ++i)
        {
            if (m_state->buffers[i].fd == fd)
            {
                queue_buffer_by_index(m_state, i);
                break;
            }
        }
    }

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

    void open_device();
    void config_format();
    void request_buffers();
    void export_buffers();
    void queue_all_buffers();

    static void queue_buffer_by_index(const std::shared_ptr<SharedState> &state, uint32_t index);

  private:
    std::string m_dev_path;
    uint32_t m_width;
    uint32_t m_height;
    uint32_t m_pix_fmt;
    uint32_t m_buffer_count;
    std::shared_ptr<SharedState> m_state;
};

#endif
#endif // V4L2_SOURCE_H