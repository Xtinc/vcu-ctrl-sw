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
 *     Only entered by enter_error_state, will immediately auto stop to STOPPED, no direct transitions allowed
 *   CLOSED:
 *     terminal state
 *
 * Key features:
 *   - Single atomic state machine for all lifecycle transitions
 *   - Import of external DMA buffers (no internal allocation)
 *   - Thread-safe asynchronous requeue by AL_TBuffer* identity
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
        DMAFd sync_desc;
    };

  public:
    /**
     * @brief Construct a V4L2Source object, open the device, and configure format.
     * @param dev Device node path, e.g. "/dev/video0"
     * @param req_width Requested width
     * @param req_height Requested height
     * @param req_fourcc Requested pixel format (FOURCC)
     * @param buf_cnt Number of buffers
     * @param multiple_planes Use multi-planar format (default: true)
     * @param sync_dev_path Xilinx sync device path. If empty or open fails, Xilinx sync is disabled (default: "")
     * @throw std::exception Throws on initialization failure
     */
    V4L2Source(const std::string &dev, int req_width, int req_height, TFourCC req_fourcc, size_t buf_cnt,
               bool multiple_planes = true, const std::string &sync_dev_path = "");
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
     * @brief Status returned by dqueue().
     *
     *   Frame:         A frame was dequeued; buffer is valid.
     *   Timeout:       No frame ready within the timeout; caller should retry.
     *   SourceChanged: V4L2_EVENT_SOURCE_CHANGE (resolution) received; caller
     *                  should stop, probe the new format, and rebuild the pipeline.
     *   Error:         Unrecoverable device error; destroy and rebuild.
     */
    enum class DequeueStatus { Frame, Timeout, SourceChanged, Error };

    struct DequeueResult
    {
        DequeueStatus status;
        AL_TBuffer *buffer; ///< Non-null only when status == Frame
    };

    /**
     * @brief Dequeue one filled frame buffer from V4L2.
     *
     * Blocks up to an internal timeout waiting for a frame or a V4L2 event.
     *
     * @return DequeueResult describing the outcome.
     */
    DequeueResult dqueue();

    /**
     * @brief Probe the current V4L2 format of a device without altering its state.
     *
     * Opens the device read-only, issues VIDIOC_G_FMT, and closes it immediately.
     * Intended for use before constructing a new V4L2Source instance to detect
     * whether the source resolution has changed.
     *
     * @param dev       Device node path.
     * @param buf_type  V4L2 buffer type (V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE or _CAPTURE).
     * @param[out] width  Current capture width, or 0 on failure.
     * @param[out] height Current capture height, or 0 on failure.
     * @return true if the format was successfully queried.
     */
    static bool probe_format(const std::string &dev, int buf_type, int &width, int &height);

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
    const size_t m_buffer_cnt;
    const int m_buf_type;
    uint32_t m_plane_size;
    std::atomic<State> m_state;

    std::mutex m_requeue_mutex;
    std::condition_variable m_requeue_cv;
    std::deque<unsigned int> m_requeue_pending;
    std::thread m_requeue_thread;

    int m_consecutive_timeout_count;
    int m_timeout_error_threshold;
    std::atomic<bool> m_buffers_queued;

    std::vector<v4l2_buffer_info> m_buffers;
    std::unique_ptr<XilinxSyncIP> m_sync_ip;
};

#endif // V4L2_SOURCE_H