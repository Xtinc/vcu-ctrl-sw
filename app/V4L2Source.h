#ifndef V4L2_SOURCE_H
#define V4L2_SOURCE_H

#include <atomic>
#include <condition_variable>
#include <deque>
#include <linux/videodev2.h>
#include <mutex>
#include <thread>

#include "DMAFd.h"

class XilinxSyncIP;

enum class DQStatus
{
    OK,
    Timeout,
    SourceChanged,
    Error
};

struct DQResult
{
    DQStatus s;
    AL_TBuffer *p;
};

/**
 * @brief V4L2Source wraps a V4L2 capture device with DMABUF import.
 *
 * This class manages the V4L2 device lifecycle, buffer import, streaming state machine,
 * and asynchronous requeueing by AL_TBuffer identity. The class is intended for
 * hardware-accelerated zero-copy pipelines (for example Zynq VCU).
 *
 * State Machine (single atomic state):
 *
 *   INIT:
 *     import_fds        -> IMPORTED
 *     stop              -> STOPPED
 *     destructor        -> CLOSED
 *   IMPORTED:
 *     start             -> STREAMING
 *     stop              -> STOPPED
 *     enter_error_state -> ERROR -> STOPPED
 *     destructor        -> CLOSED
 *   STREAMING:
 *     queue/dqueue/requeue_worker active
 *     stop              -> STOPPED
 *     enter_error_state -> ERROR -> STOPPED
 *     destructor        -> CLOSED
 *   STOPPED:
 *     import_fds        -> IMPORTED
 *     enter_error_state -> ERROR -> STOPPED
 *     stop              -> STOPPED
 *     destructor        -> CLOSED
 *   ERROR:
 *     Entered via enter_error_state(), then drained to STOPPED by stop()
 *   CLOSED:
 *     terminal state
 *
 * Key features:
 *   - Single atomic state machine for all lifecycle transitions
 *   - Import of external DMA buffers (no internal allocation)
 *   - Thread-safe asynchronous requeue by AL_TBuffer* identity
 *   - Strict error handling and resource cleanup
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
        DMAFd sync_desc;
    };

  public:
    /**
        * @brief Open capture/sub-device nodes and configure capture format.
     * @param dev Device node path, e.g. "/dev/video0"
     * @param sub_dev Subdevice node path, e.g. "/dev/v4l-subdev0"
     * @param req_width Requested width
     * @param req_height Requested height
     * @param req_fourcc Requested pixel format (FOURCC)
     * @param buf_cnt Number of buffers
     * @param multiple_planes Use multi-planar format (default: true)
     * @param sync_dev_path Xilinx sync device path. If empty or open fails, Xilinx sync is disabled (default: "")
    * @throw std::exception on initialization failure
     */
    V4L2Source(const std::string &dev, const std::string &sub_dev, int req_width, int req_height, TFourCC req_fourcc,
               size_t buf_cnt, bool multiple_planes = true, const std::string &sync_dev_path = "");
    ~V4L2Source();
    V4L2Source(const V4L2Source &) = delete;
    V4L2Source &operator=(const V4L2Source &) = delete;

    /**
     * @brief Import externally allocated DMA buffers, recreate V4L2 queue resources, and pre-queue buffers.
     * @param buffers Array of DMAFd entries. Must match buf_cnt.
     * @return true on success, false on failure
     */
    bool import_fds(DMAFdArray &&buffers);

    /**
     * @brief Start V4L2 streaming (STREAMON), transition to STREAMING state.
     * @note start() is valid only after a successful import_fds().
     * @return true on success, false on failure
     */
    bool start();

    /**
     * @brief Stop V4L2 streaming and fully release V4L2 queue resources, transition to STOPPED state.
     * @note After stop(), import_fds() must be called again before start().
     * @return true on success, false on failure
     */
    bool stop();

    /**
     * @brief Re-queue a previously dequeued buffer back to the V4L2 input queue.
     *
     * The API is asynchronous: it only enqueues an index into an internal pending queue.
     * Actual VIDIOC_QBUF is executed by an internal worker thread.
     *
     * @param buffer Buffer identity pointer returned by dqueue().
     * @return true if accepted for requeue, false on invalid state/input or lookup failure
     */
    bool queue(AL_TBuffer const *buffer);

    /**
     * @brief Dequeue one filled frame buffer from V4L2.
     *
     * Blocks up to an internal timeout waiting for a frame or a V4L2 event.
     *
    * @return DQResult containing dequeue status and buffer pointer.
     */
    DQResult dqueue();

    /**
     * @brief Query the active source format from a V4L2 sub-device.
     *
     * This checks if a video source is connected and active, and returns its resolution.
     * Useful for dynamic sources like HDMI/SDI inputs where the resolution is not fixed.
     *
     * @param subdev    Sub-device path (e.g., "/dev/v4l-subdev0").
     * @param pad       Source pad number (usually 0).
     * @param[out] width  Source width in pixels, or 0 on failure.
     * @param[out] height Source height in pixels, or 0 on failure.
     * @return true if a source is active and format was retrieved, false otherwise.
     */
    static bool probe_subdev_format(const std::string &subdev, int pad, int &width, int &height);

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
    DQStatus handle_pollpri_event(int revents);
    DQResult handle_timeout(const char *reason);

    int m_fd;
    int m_sub_fd;
    const size_t m_buffer_cnt;
    const int m_buf_type;
    uint32_t m_plane_size;
    std::atomic<State> m_state;

    std::mutex m_requeue_mutex;
    std::condition_variable m_requeue_cv;
    std::deque<unsigned int> m_requeue_pending;
    std::thread m_requeue_thread;

    int m_consecutive_timeout_count;
    std::atomic<bool> m_buffers_queued;

    std::vector<v4l2_buffer_info> m_buffers;
    std::unique_ptr<XilinxSyncIP> m_sync_ip;
};

#endif // V4L2_SOURCE_H