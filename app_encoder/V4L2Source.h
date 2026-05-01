#ifndef V4L2_SOURCE_H
#define V4L2_SOURCE_H

extern "C"
{
#include "lib_common/FourCC.h"
}

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <linux/videodev2.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

/**
 * @brief V4L2Source encapsulates a V4L2 video capture device using external DMA buffer import (DMABUF).
 *
 * This class manages the V4L2 device lifecycle, buffer import, streaming state machine,
 * and safe multi-threaded requeueing of buffers. It is designed for robust integration
 * with hardware-accelerated video pipelines (e.g., Zynq VCU) where buffer management
 * and state transitions must be strictly controlled.
 *
 * State Machine (single atomic state):
 *
 *   INIT:
 *     import_fds        -> IMPORTED
 *     destructor        -> CLOSED
 *   IMPORTED:
 *     start             -> STREAMING
 *     stop              -> STOPPED
 *     enter_error_state -> ERROR
 *     destructor        -> CLOSED
 *   STREAMING:
 *     queue_idx/dqueue_idx/requeue_worker active
 *     stop              -> STOPPED
 *     enter_error_state -> ERROR
 *     destructor        -> CLOSED
 *   STOPPED:
 *     import_fds        -> IMPORTED
 *     start             -> STREAMING
 *     enter_error_state -> ERROR
 *     destructor        -> CLOSED
 *   ERROR:
 *     stop (best-effort streamoff) -> STOPPED
 *     destructor        -> CLOSED
 *   CLOSED:
 *     terminal state
 *
 * Key features:
 *   - Single atomic state machine for all lifecycle transitions
 *   - Import of external DMA buffers (no internal allocation)
 *   - Thread-safe asynchronous buffer requeueing
 *   - Strict error handling and resource cleanup
 *   - Designed for use with hardware encoders/decoders and zero-copy pipelines
 */
class V4L2Source
{
    enum State
    {
        INIT,
        IMPORTED,
        STREAMING,
        STOPPED,
        ERROR,
        CLOSED,
    };

    struct v4l2_buffer_info
    {
        v4l2_buffer buf;
        v4l2_plane plane[1];
    };

  public:
    /**
     * @brief Construct a V4L2Source object, open the device, configure format, and request buffers.
     * @param dev Device node path, e.g. "/dev/video0"
     * @param req_width Requested width
     * @param req_height Requested height
     * @param req_fourcc Requested pixel format (FOURCC)
     * @param buf_cnt Number of buffers
     * @param multiple_planes Use multi-planar format (default: true)
     * @throw std::exception Throws on initialization failure
     */
    V4L2Source(const std::string &dev, int req_width, int req_height, TFourCC req_fourcc, int buf_cnt,
               bool multiple_planes = true);
    ~V4L2Source();
    V4L2Source(const V4L2Source &) = delete;
    V4L2Source &operator=(const V4L2Source &) = delete;

    /**
     * @brief Import externally allocated DMA buffer file descriptors and initialize V4L2 buffers.
     * @param fds Array of DMA buffer file descriptors. Must match buf_cnt.
     * @return true on success, false on failure
     */
    bool import_fds(const std::vector<int> &fds);

    /**
     * @brief Start V4L2 streaming (STREAMON), transition to STREAMING state.
     * @return true on success, false on failure
     */
    bool start();

    /**
     * @brief Stop V4L2 streaming (STREAMOFF), transition to STOPPED state.
     * @return true on success, false on failure
     */
    bool stop();

    /**
     * @brief Request to requeue a buffer by index (asynchronous, thread-safe).
     * @param idx Buffer index (0~buf_cnt-1)
     * @return true on success, false on failure
     */
    bool queue_idx(unsigned int idx);

    /**
     * @brief Dequeue a completed buffer index (blocking, with timeout).
     * @param[out] idx Returns the dequeued buffer index
     * @return true on success, false on failure or timeout
     */
    bool dqueue_idx(unsigned int &idx);

  private:
    static const char *state_to_cstr(State state);
    static bool can_import(State state);
    static bool can_start(State state);
    static bool can_stop(State state);
    static bool can_queue(State state);
    static bool can_dequeue(State state);

    void enter_error_state();
    bool qbuf_idx(unsigned int idx);
    void stop_requeue_worker();
    void requeue_worker_loop();

    int m_fd;
    const int m_buffer_count;
    const int m_buf_type;
    uint32_t m_plane_size;
    std::atomic<State> m_state;

    std::mutex m_requeue_mutex;
    std::condition_variable m_requeue_cv;
    std::deque<unsigned int> m_requeue_pending;
    std::thread m_requeue_thread;

    int m_consecutive_timeout_count;
    int m_timeout_error_threshold;

    std::vector<v4l2_buffer_info> m_buffers;
};

#endif // V4L2_SOURCE_H