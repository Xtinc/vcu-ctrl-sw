#ifndef REALTIME_ENCODER_H
#define REALTIME_ENCODER_H

#include "MemMgr.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

extern "C"
{
#include "lib_common_enc/EncChanParam.h"
#include "lib_common_enc/Settings.h"
#include "lib_encode/lib_encoder.h"
}

inline std::string FOURCC2STR(TFourCC fourcc)
{
    char buf[5]{};
    buf[0] = static_cast<char>(fourcc & 0xFF);
    buf[1] = static_cast<char>((fourcc >> 8) & 0xFF);
    buf[2] = static_cast<char>((fourcc >> 16) & 0xFF);
    buf[3] = static_cast<char>((fourcc >> 24) & 0xFF);
    return std::string(buf);
}

inline TFourCC STR2FOURCC(const std::string &str)
{
    if (str.size() != 4)
    {
        return 0;
    }
    return static_cast<uint32_t>(str[0]) | (static_cast<uint32_t>(str[1]) << 8) |
           (static_cast<uint32_t>(str[2]) << 16) | (static_cast<uint32_t>(str[3]) << 24);
}

struct EncoderConfig
{
    AL_EProfile profile;        // AVC/HEVC Profile
    uint8_t level;              // Level (e.g., H.265 Level 5.1)
    uint8_t tier;               // 0 = Main Tier, 1 = High Tier (HEVC only)
    uint16_t width;             // Frame width
    uint16_t height;            // Frame height
    AL_EChromaMode chroma_mode; // Chroma sampling format
    uint8_t bit_depth;          // Bit depth (8 or 10)
    AL_ERateCtrlMode rc_mode;   // Rate control mode
    uint32_t target_bitrate;    // Target bitrate in bps
    uint32_t max_bitrate;       // VBR peak bitrate in bps, 0 = same as target
    int16_t initial_qp;         // Initial QP (fixed QP in CQP mode)
    uint16_t framerate;         // Frame rate numerator
    uint16_t clk_ratio;         // Frame rate denominator, final frame rate = framerate*1000/clk_ratio
    uint16_t gop_length;        // GOP length (number of frames between two IDR/I frames)
    uint32_t freq_idr;          // IDR forced insertion frequency (0 = only GOP first frame)
    bool low_delay_mode;        // true = low-latency P-frame GOP (no B-frames, minimal encode/decode latency)
    uint8_t num_b;              // Number of B-frames in GOP (ignored if low_delay_mode is true)
    uint32_t num_src_bufs;      // Number of input source frame buffers
    uint32_t num_stream_bufs;   // Number of output stream buffers
    std::string enc_dev_path;   // Encoder device node (e.g., "/dev/allegroIP")
    std::string dma_dev_path;   // DMAProxy device node (e.g., "/dev/dmaproxy")
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

    TFourCC SRC_FourCC() const;
    uint8_t SRC_bitdepth() const;
    AL_EChromaMode SRC_chroma() const;
    AL_TDimension SRC_resolution() const;

  protected:
    // CTOR throw std::runtime_error on initialization failure (e.g., hardware init, encoder creation)
    explicit RTEncoderBase(const EncoderConfig &cfg, EncodedFrameCallback cb);

  private:
    static void sdk_callback(void *pUserParam, AL_TBuffer *pStream, AL_TBuffer const *pSrc, int iLayerID);
    void on_encoded_frame(AL_TBuffer *pStream, AL_TBuffer const *pSrc);

    void init_settings(AL_TEncSettings &settings) const;
    void init_source_buf_pool();
    void init_stream_buf_pool();
    void push_stream_buffers();
    virtual void release_sources(AL_TBuffer const *pSrc) = 0;

  protected:
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

  private:
    AL_TDimension m_src_dim;
};

template <> class RTEncoder<SourceMode::V4L2> : public RTEncoderBase
{
  public:
    using SourceReleaseCallback = std::function<void(int idx, void *userData)>;

    RTEncoder(const EncoderConfig &cfg, EncodedFrameCallback cb);
    ~RTEncoder() override;

    void set_release_callback(SourceReleaseCallback releaseCb, void *userData = nullptr);
    std::vector<int> acquire_dma_fds(int count);
    bool submit_dma_index(int idx);

  private:
    void release_sources(AL_TBuffer const *pSrc) override;

  private:
    std::mutex m_idx_mutex;
    std::vector<AL_TBuffer *> m_idx_map;
    void *m_usr_data;
    SourceReleaseCallback m_release_cb;
};

#endif // REALTIME_ENCODER_H
