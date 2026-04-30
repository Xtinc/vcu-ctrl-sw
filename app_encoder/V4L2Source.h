#ifndef V4L2_SOURCE_H
#define V4L2_SOURCE_H

extern "C"
{
#include "lib_common/FourCC.h"
}

#include <atomic>
#include <linux/videodev2.h>
#include <string>
#include <vector>

class V4L2Source
{
    struct v4l2_buffer_info
    {
        v4l2_buffer buf;
        v4l2_plane plane[1];
    };

  public:
    // CTOR throw exception if initialization fails
    V4L2Source(const std::string &dev, int req_width, int req_height, TFourCC req_fourcc, int buf_cnt,
               bool multiple_planes = true);
    ~V4L2Source();
    V4L2Source(const V4L2Source &) = delete;
    V4L2Source &operator=(const V4L2Source &) = delete;

    bool import_fds(const std::vector<int> &fds);
    bool start();
    bool stop();

    bool queue_idx(int idx);
    int dequeue_idx();

  private:
    int m_fd;
    const std::string m_dev;
    const int m_width;
    const int m_height;
    const int m_buffer_count;
    const int m_buf_type;
    int m_pix_fmt;
    std::atomic<bool> m_streaming;
    std::vector<v4l2_buffer_info> m_buffers;
};

#endif // V4L2_SOURCE_H