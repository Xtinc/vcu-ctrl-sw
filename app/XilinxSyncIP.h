#ifndef XILINX_SYNC_IP_H
#define XILINX_SYNC_IP_H

extern "C"
{
#include "lib_sync_ip/xilinx-v4l2-controls.h"
#include "lib_sync_ip/xvfbsync.h"
}

#include "V4L2DMAFd.h"

/**
 * @brief XilinxSyncIP manages Xilinx framebuffer synchronization IP core.
 *
 * This class provides low-latency synchronization between video capture and encoder
 * using Xilinx hardware IP. If device open fails, object is created but all operations
 * will fail safely.
 */
class XilinxSyncIP
{
  public:
    /**
     * @brief Open Xilinx sync device.
     * @param dev Device path (e.g., "/dev/xlnxsync0")
     * @note Does not throw. Check init() return value to verify device is operational.
     */
    XilinxSyncIP(const std::string &dev) noexcept;
    ~XilinxSyncIP() noexcept;

    XilinxSyncIP(const XilinxSyncIP &) = delete;
    XilinxSyncIP &operator=(const XilinxSyncIP &) = delete;

    /**
     * @brief Initialize sync IP channels.
     * @return true on success, false on failure
     */
    bool init() noexcept;

    /**
     * @brief Start sync IP streaming.
     * @return true on success, false on failure
     */
    bool start() noexcept;

    /**
     * @brief Stop and depopulate sync IP channels. Safe to call more than once.
     * @return true on success, false on failure
     */
    bool stop() noexcept;

    /**
     * @brief Add a DMA buffer to sync IP for tracking.
     * @param buffer_desc DMA buffer description
     * @return true on success, false on failure
     */
    bool add_buffer(const DMAFd &buffer_desc) noexcept;

  private:
    int m_fd;
    SyncIp m_syncip;
    SyncChannel m_sync_chan;
    EncSyncChannel m_enc_sync_chan;
    bool m_initialized;
};

#endif
