#ifndef REALTIME_ENCODER_H
#define REALTIME_ENCODER_H

#include "DMAFd.h"
#include "MemMgr.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

extern "C"
{
#include "lib_common_enc/EncChanParam.h"
#include "lib_common_enc/Settings.h"
#include "lib_encode/lib_encoder.h"
}

struct EncoderConfig
{
    AL_EProfile profile = AL_PROFILE_HEVC_MAIN;   // AVC/HEVC Profile
    uint8_t level = 51;                           // Level (e.g., H.265 Level 5.1)
    uint8_t tier = 0;                             // 0 = Main Tier, 1 = High Tier (HEVC only)
    uint16_t width = 3840;                        // Frame width
    uint16_t height = 2160;                       // Frame height
    AL_EChromaMode chroma_mode = AL_CHROMA_4_2_0; // Chroma sampling format
    uint8_t bit_depth = 8;                        // Bit depth (8 or 10)
    AL_ERateCtrlMode rc_mode = AL_RC_CBR;         // Rate control mode
    uint32_t target_bitrate = 25 * 1000 * 1000;   // Target bitrate in bps
    uint32_t max_bitrate = 0;                     // VBR peak bitrate in bps, 0 = same as target
    int16_t initial_qp = 35;                      // Initial QP (fixed QP in CQP mode)
    uint16_t framerate = 60;                      // Frame rate numerator
    uint16_t clk_ratio = 1000;                    // Frame rate denominator, final frame rate = framerate*1000/clk_ratio
    uint16_t gop_length = 60;                     // GOP length (number of frames between two IDR/I frames)
    uint32_t freq_idr = 240;                      // IDR forced insertion frequency (0 = only GOP first frame)
    uint8_t num_b = 2;                            // Number of B-frames in GOP (ignored if low_delay_mode is true)
    uint32_t num_src_bufs = 4;                    // Number of input source frame buffers
    uint32_t num_stream_bufs = 4;                 // Number of output stream buffers
    std::string enc_dev_path = "/dev/allegroIP";  // Encoder device node (e.g., "/dev/allegroIP")
    std::string dma_dev_path = "/dev/dmaproxy";   // DMAProxy device node (e.g., "/dev/dmaproxy")
    bool low_delay_mode = false; // true = low-latency P-frame GOP (no B-frames, minimal encode/decode latency)
};

enum class SourceMode
{
    FILE,
    V4L2
};

class RTEncoderBase
{
  public:
    using EncodedFrameCallback = std::function<void(const uint8_t *pData, size_t size)>;
    virtual ~RTEncoderBase();

    RTEncoderBase(const RTEncoderBase &) = delete;
    RTEncoderBase &operator=(const RTEncoderBase &) = delete;
    RTEncoderBase(RTEncoderBase &&) = delete;
    RTEncoderBase &operator=(RTEncoderBase &&) = delete;

    // throw std::runtime_error on failure (e.g., EOS timeout)
    void flush();
    void request_IDR();
    bool set_bitrate(uint32_t uTargetBitRate, uint32_t uMaxBitRate = 0);
    bool set_framerate(uint32_t uFrameRate, uint32_t uClkRatio = 1000);
    bool set_resolution(uint32_t uWidth, uint32_t uHeight);

    TFourCC src_fourCC() const;
    uint8_t src_bitdepth() const;
    AL_EChromaMode src_chroma() const;
    AL_TDimension src_resolution() const;
    std::pair<double, double> fps() const;

  protected:
    // CTOR throw std::runtime_error on initialization failure (e.g., hardware init, encoder creation)
    explicit RTEncoderBase(const EncoderConfig &cfg, EncodedFrameCallback cb);

  private:
    static void sdk_callback(void *pUserParam, AL_TBuffer *pStream, AL_TBuffer const *pSrc, int iLayerID);
    void on_encoded_frame(AL_TBuffer *pStream, AL_TBuffer const *pSrc);
    void update_frame_rate();

    void init_settings(AL_TEncSettings &settings) const;
    void init_source_buf_pool();
    void init_stream_buf_pool();
    void push_stream_buffers();
    virtual void release_sources(AL_TBuffer const *pSrc) = 0;

  protected:
    mutable std::mutex m_fps_mutex;
    double m_fps;
    double m_bitrate;
    uint32_t m_frame_count;
    uint32_t m_bytes_count;
    std::chrono::steady_clock::time_point m_fps_last_time;

    mutable std::mutex m_cfg_mutex;
    EncoderConfig m_cfg;
    EncodedFrameCallback m_callback;

    DMAProxy m_dma_proxy;
    AL_TAllocator *m_pAllocator;
    AL_IEncScheduler *m_pScheduler;
    AL_HEncoder m_hEnc;

    std::unique_ptr<PixMapBufPool> m_source_buf_pool;
    std::unique_ptr<GenericBufPool> m_stream_buf_pool;

    AL_TPicFormat m_pic_format;
    TFourCC m_src_fourcc;

    std::atomic<bool> m_stopped;
    std::mutex m_eos_mutex;
    std::condition_variable m_eos_cond;
    bool m_eos_signaled;

    std::atomic<bool> m_error;
    bool m_lib_initialized;
};

template <SourceMode mode> class RTEncoder;
using RTEncoderFile = RTEncoder<SourceMode::FILE>;
using RTEncoderV4L2 = RTEncoder<SourceMode::V4L2>;

template <> class RTEncoder<SourceMode::FILE> : public RTEncoderBase
{
  public:
    RTEncoder(const EncoderConfig &cfg, EncodedFrameCallback cb);
    ~RTEncoder() override = default;

    AL_TBuffer *acquire_source_buffer();
    bool submit_source_buffer(AL_TBuffer *pBuf);

  private:
    void release_sources(AL_TBuffer const *pSrc) override;
};

template <> class RTEncoder<SourceMode::V4L2> : public RTEncoderBase
{
  public:
    struct Slot
    {
        AL_TBuffer *buf = nullptr;
        bool inflight = false;
        DMAFd desc{};
    };

    using SourceReleaseCallback = std::function<void(int idx)>;

    RTEncoder(const EncoderConfig &cfg, EncodedFrameCallback cb);
    ~RTEncoder() override;

    void set_release_callback(SourceReleaseCallback releaseCb);
    std::vector<DMAFd> acquire_dma_buffers(unsigned int count);
    bool submit_dma_index(unsigned int idx);

  private:
    void release_sources(AL_TBuffer const *pSrc) override;
    std::mutex m_idx_mutex;
    std::vector<Slot> m_slots;
    SourceReleaseCallback m_release_cb;
};

#endif // REALTIME_ENCODER_H
