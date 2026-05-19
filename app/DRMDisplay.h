#ifndef DRM_DISPLAY_H
#define DRM_DISPLAY_H

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_decode/lib_decode.h"
}

#include <cstdint>
#include <functional>
#include <string>

/**
 * @brief Configuration parameters for DRMDisplay.
 */
struct DRMDisplayConfig
{
    std::string drm_device   = "/dev/dri/card0"; ///< DRM device node path.
    int connector_id         = -1;               ///< KMS connector ID (-1 = auto-detect first connected).
    int crtc_id              = -1;               ///< KMS CRTC ID (-1 = auto-detect from connector).
    int plane_id             = -1;               ///< KMS plane ID (-1 = auto-detect overlay or primary plane).
    bool enable_vsync        = false;            ///< Block in show() until page-flip completes.
    bool force_mode_set      = false;            ///< Force drmModeSetCrtc even if a mode is already active.
};

/**
 * @brief Zero-copy DRM/KMS display for Xilinx ZYNQ VCU decoded frames.
 *
 * DRMDisplay presents decoded AL_TBuffer frames directly onto a DRM overlay
 * (or primary) plane via PRIME DMA-buf import — no CPU copy is required.
 *
 * ### Buffer lifecycle (double-buffer scheme)
 *
 * To prevent the decoder from overwriting a frame that is still being
 * scanned out by the display controller, DRMDisplay holds a reference to the
 * currently displayed frame and only releases it when the *next* frame
 * replaces it:
 *
 *  1. show() is called with a new frame + a @p on_frame_return callback.
 *  2. The frame's DMA-buf fd is imported with drmPrimeFDToHandle.
 *  3. A DRM framebuffer object is created with drmModeAddFB2.
 *  4. The plane is updated with drmModeSetPlane.
 *  5. The *previous* frame's fb is destroyed (drmModeRmFB) and its GEM
 *     handles are closed; then its @p on_frame_return callback is invoked,
 *     which typically calls RTDecoder::return_display_frame().
 *  6. The new frame + fb info is stored for the next iteration.
 *
 * This scheme requires that DecoderConfig::manual_frame_return is @c true in
 * the companion RTDecoder so that the decoder does not reclaim the buffer
 * before DRMDisplay releases it.
 *
 * ### Supported pixel formats
 * | Allegro FourCC | DRM format     | Description                     |
 * |----------------|----------------|---------------------------------|
 * | NV12           | DRM_FORMAT_NV12| 8-bit YUV 4:2:0 semi-planar     |
 * | XV15           | DRM_FORMAT_XV15| 10-bit YUV 4:2:0 (Xilinx packed)|
 * | NV16           | DRM_FORMAT_NV16| 8-bit YUV 4:2:2 semi-planar     |
 * | XV20           | DRM_FORMAT_XV20| 10-bit YUV 4:2:2 (Xilinx packed)|
 *
 * ### Typical usage
 * @code
 *   DRMDisplayConfig disp_cfg;
 *   disp_cfg.drm_device = "/dev/dri/card0";
 *   DRMDisplay display(disp_cfg);
 *
 *   DecoderConfig dec_cfg;
 *   dec_cfg.manual_frame_return = true;   // required!
 *   dec_cfg.low_delay_mode       = true;
 *
 *   RTDecoder decoder(dec_cfg,
 *       [&display, &decoder](AL_TBuffer* frame, const AL_TInfoDecode& info) {
 *           display.show(frame, info, [&decoder, frame] {
 *               decoder.return_display_frame(frame);
 *           });
 *       });
 * @endcode
 *
 * @note Non-copyable, non-movable.
 * @throws std::runtime_error  If DRM device cannot be opened or no suitable
 *                             connector/CRTC/plane is found.
 */
class DRMDisplay
{
  public:
    /**
     * @brief Callback invoked when the display is finished with a frame.
     *
     * Typically wraps RTDecoder::return_display_frame() to return the DMA
     * buffer back to the decoder's rec pool.
     */
    using FrameReturnCallback = std::function<void()>;

    /**
     * @brief Construct and initialise the DRM display pipeline.
     *
     * Opens the DRM device, enumerates connectors/CRTCs/planes, optionally
     * sets the display mode, and prepares for zero-copy frame presentation.
     *
     * @param cfg  Display configuration.
     * @throws std::runtime_error  On DRM open / resource enumeration failure.
     */
    explicit DRMDisplay(const DRMDisplayConfig &cfg);

    /**
     * @brief Release the currently held frame (if any) and close the DRM device.
     */
    ~DRMDisplay();

    DRMDisplay(const DRMDisplay &)            = delete;
    DRMDisplay &operator=(const DRMDisplay &) = delete;
    DRMDisplay(DRMDisplay &&)                 = delete;
    DRMDisplay &operator=(DRMDisplay &&)      = delete;

    /**
     * @brief Present a decoded frame on the DRM plane (zero-copy).
     *
     * Must be called from a single thread (typically the decoder callback
     * thread). Blocks briefly to import the DMA-buf and call drmModeSetPlane;
     * if @c enable_vsync is set it also waits for the page-flip event.
     *
     * The @p on_frame_return callback is stored and invoked the *next* time
     * show() is called (i.e. when the current frame is superseded) or when
     * release_current() / destructor is called.
     *
     * @param frame            Decoded picture buffer.  Must carry
     *                         AL_TPixMapMetaData.  Must not be null.
     * @param info             Frame display metadata (dimensions, crop, etc.).
     * @param on_frame_return  Called when this frame can be returned to the
     *                         decoder.  Must not be null.
     * @return @c true on success; @c false if the DRM import or plane update
     *         failed (the previous frame is still displayed).
     */
    bool show(AL_TBuffer *frame, const AL_TInfoDecode &info, FrameReturnCallback on_frame_return);

    /**
     * @brief Release the currently displayed frame immediately.
     *
     * Calls the frame's on_frame_return callback and tears down the associated
     * DRM framebuffer.  Safe to call even if no frame is held.
     */
    void release_current();

  private:
    // Internal representation of a frame held by the display.
    struct HeldFrame
    {
        AL_TBuffer       *buf         = nullptr;
        uint32_t          fb_id       = 0;
        uint32_t          gem_handles[4] = {};
        int               num_gem     = 0;
        FrameReturnCallback on_return;
    };

    void     init_drm();
    void     close_drm();
    uint32_t allegro_fourcc_to_drm(uint32_t al_fourcc) const;
    void     free_held_frame(HeldFrame &f);
    bool     do_set_plane(uint32_t fb_id, uint32_t w, uint32_t h);
    bool     wait_for_flip();

    DRMDisplayConfig m_cfg;

    int      m_drm_fd   {-1};
    uint32_t m_conn_id  {0};
    uint32_t m_crtc_id  {0};
    uint32_t m_plane_id {0};
    int      m_pipe     {0};         ///< Pipe index (0-based) for vblank requests.
    bool     m_first_frame {true};   ///< True until first frame is shown (triggers mode-set).

    HeldFrame m_held; ///< Frame currently displayed (held until next frame).
};

#endif // DRM_DISPLAY_H
