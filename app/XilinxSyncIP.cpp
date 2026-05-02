#include "XilinxSyncIP.h"
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

extern "C"
{
#include "lib_rtos/message.h"
}

static constexpr int HORIZONTAL_ALIGNMENT = 64;
static constexpr int VERTICAL_ALIGNMENT = 32;

XilinxSyncIP::XilinxSyncIP(const std::string &dev) noexcept : m_fd(-1), m_syncip{}, m_sync_chan{}, m_enc_sync_chan{}
{
    m_fd = ::open(dev.c_str(), O_RDWR);
    if (m_fd < 0)
    {
        VIDEO_ERROR_PRINT("Failed to open Xilinx sync device '%s': %s", dev.c_str(), std::strerror(errno));
    }
}

XilinxSyncIP::~XilinxSyncIP() noexcept
{
    if (m_fd >= 0)
    {
        xvfbsync_enc_sync_chan_depopulate(&m_enc_sync_chan);
        ::close(m_fd);
        m_fd = -1;
    }
}

bool XilinxSyncIP::init() noexcept
{
    if (m_fd < 0)
    {
        VIDEO_ERROR_PRINT("Sync device not opened");
        return false;
    }

    if (xvfbsync_syncip_chan_populate(&m_syncip, &m_sync_chan, m_fd) != 0)
    {
        VIDEO_ERROR_PRINT("Failed to populate sync IP channel");
        return false;
    }

    if (xvfbsync_enc_sync_chan_populate(&m_enc_sync_chan, &m_sync_chan, HORIZONTAL_ALIGNMENT, VERTICAL_ALIGNMENT) != 0)
    {
        VIDEO_ERROR_PRINT("Failed to populate encoder sync channel");
        return false;
    }

    return true;
}

bool XilinxSyncIP::start() noexcept
{
    if (m_fd < 0)
    {
        VIDEO_ERROR_PRINT("Sync device not opened");
        return false;
    }

    ChannelIntr intr_mask{};
    intr_mask.prod_lfbdone = 1;
    intr_mask.prod_cfbdone = 1;
    intr_mask.cons_lfbdone = 1;
    intr_mask.cons_cfbdone = 1;

    if (xvfbsync_enc_sync_chan_enable(&m_enc_sync_chan) != 0)
    {
        VIDEO_ERROR_PRINT("Failed to enable sync channel");
        return false;
    }

    if (xvfbsync_enc_sync_chan_set_intr_mask(&m_enc_sync_chan, &intr_mask) != 0)
    {
        VIDEO_ERROR_PRINT("Failed to set interrupt mask");
        return false;
    }

    return true;
}

bool XilinxSyncIP::add_buffer(const DMAFd &buffer_desc) noexcept
{
    if (m_fd < 0)
    {
        VIDEO_ERROR_PRINT("Sync device not opened");
        return false;
    }

    XLNXLLBuf *xlnxll_buf = xvfbsync_xlnxll_buf_new();
    if (!xlnxll_buf)
    {
        VIDEO_ERROR_PRINT("Failed to allocate buffer descriptor");
        return false;
    }

    // Configure buffer metadata
    xlnxll_buf->dma_fd = buffer_desc.dma_fd;
    xlnxll_buf->t_dim.i_width = buffer_desc.width;
    xlnxll_buf->t_dim.i_height = buffer_desc.height;
    xlnxll_buf->t_fourcc = buffer_desc.fourcc;
    xlnxll_buf->t_planes[PLANE_Y].i_offset = buffer_desc.y_offset;
    xlnxll_buf->t_planes[PLANE_Y].i_pitch = buffer_desc.y_pitch;
    xlnxll_buf->t_planes[PLANE_UV].i_offset = buffer_desc.uv_offset;
    xlnxll_buf->t_planes[PLANE_UV].i_pitch = buffer_desc.uv_pitch;
    xlnxll_buf->t_planes[PLANE_MAP_Y].i_offset = 0;
    xlnxll_buf->t_planes[PLANE_MAP_Y].i_pitch = 0;
    xlnxll_buf->t_planes[PLANE_MAP_UV].i_offset = 0;
    xlnxll_buf->t_planes[PLANE_MAP_UV].i_pitch = 0;

    if (xvfbsync_enc_sync_chan_add_buffer(&m_enc_sync_chan, xlnxll_buf) != 0)
    {
        VIDEO_ERROR_PRINT("Failed to add buffer to sync channel");
        return false;
    }

    // Check and clear channel error status
    ChannelStatus *channel_status = m_sync_chan.channel_status;
    pthread_mutex_lock(&channel_status->mutex);
    const bool has_error = channel_status->err_cond != 0;
    if (has_error)
    {
        channel_status->err_cond = 0;
    }
    pthread_mutex_unlock(&channel_status->mutex);

    if (has_error)
    {
        if (xvfbsync_syncip_reset_err_status(&m_enc_sync_chan, &channel_status->err) != 0)
        {
            VIDEO_ERROR_PRINT("Failed to clear channel error status");
            return false;
        }
    }

    return true;
}
