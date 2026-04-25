// SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_common/Error.h"
#include "lib_common/FourCC.h"
#include "lib_common_enc/EncChanParam.h"
#include "lib_common_enc/Settings.h"
#include "lib_encode/lib_encoder.h"
}

#include "lib_app/BufPool.h"
#include "lib_app/PixMapBufPool.h"
#include "lib_app/video_utils.h"

#include "DMAProxy.h"

struct EncoderConfig
{
    /* ---- 编解码器 ---- */
    AL_EProfile eProfile = AL_PROFILE_HEVC_MAIN; /*!< 编码 Profile，HEVC/AVC 通过 Profile 区分 */
    uint8_t uLevel = 51;                         /*!< Level，如 H.265 Level 5.1 */
    uint8_t uTier = 0;                           /*!< 0 = Main Tier，1 = High Tier（HEVC only）*/

    /* ---- 分辨率 ---- */
    uint16_t width = 1920;
    uint16_t height = 1080;

    /* ---- 源图像格式 ---- */
    AL_EChromaMode eChromaMode = AL_CHROMA_4_2_0; /*!< 色度采样格式 */
    uint8_t uBitDepth = 8;                        /*!< 位深，8 或 10 */

    /* ---- 码率控制 ---- */
    AL_ERateCtrlMode eRCMode = AL_RC_CBR; /*!< 码率控制模式 */
    uint32_t uTargetBitRate = 4000000;    /*!< 目标码率，单位 bps */
    uint32_t uMaxBitRate = 0;             /*!< VBR 峰值码率，0 = 与目标相同 */
    int16_t iInitialQP = 30;              /*!< 初始 QP（CQP 时即固定 QP） */
    uint16_t uFrameRate = 30;             /*!< 帧率分子 */
    uint16_t uClkRatio = 1000;            /*!< 帧率分母，最终帧率 = uFrameRate*1000/uClkRatio */

    /* ---- GOP 结构 ---- */
    uint16_t uGopLength = 30;   /*!< GOP 长度（两个 IDR/I 帧之间的帧数） */
    uint32_t uFreqIDR = 0;      /*!< IDR 强制插入频率（0 = 仅 GOP 首帧） */
    bool bLowDelayMode = false; /*!< true = 低延迟 P 帧 GOP（无 B 帧，编解码延迟最小） */
    uint8_t uNumB = 0;          /*!< GOP 内 B 帧数量（bLowDelayMode 为 true 时忽略） */

    /* ---- 缓冲区数量 ---- */
    uint32_t uNumSrcBufs = 4;    /*!< 输入源帧缓冲数量 */
    uint32_t uNumStreamBufs = 4; /*!< 输出码流缓冲数量 */

    /* ---- 硬件设备路径 ---- */
    std::string sDevicePath = "/dev/allegroIP"; /*!< ZYNQ VCU 设备节点 */
    std::string sDMAProxyPath = "/dev/dmaproxy"; /*!< DMAProxy 设备节点 */
};

/*****************************************************************************
 * RealtimeEncoder
 * ---------------------------------------------------------------------------
 * 对 Allegro VCU SDK 编码器 API 的轻量封装，提供：
 *   - 基于 DMA 分配器 + MCU Scheduler 的硬件初始化
 *   - acquireSourceBuffer() / submitSourceBuffer() 接口供外部直接写入 DMA 缓冲
 *   - 编码完成后通过回调返回码流数据
 *   - flush() 触发 EOS 并等待编码器排空
 *
 * 引用计数约定（与 SDK 原工程一致）：
 *   GetBuffer 使引用计数 +1（用户引用），SDK 内部在 Process/PutStreamBuffer 时
 *   再 +1（SDK 引用），用户提交后立即 Unref 释放自身引用，SDK 在完成后通过
 *   source-release / stream-release 回调释放其引用，引用归零时缓冲自动回池。
 *
 * 线程安全说明：
 *   =acquireSourceBuffer() / submitSourceBuffer() / flush() 应在
 *   同一线程（生产者线程）中调用。
 *   EncodedFrameCallback 从 SDK 内部回调线程调用，请勿在回调中调用 pushFrame()。
 *****************************************************************************/
class RealtimeEncoder
{
  public:
    enum WorkMode
    {
        FILE, /*!< 从文件读取 YUV 原始帧进行编码 */
        V4L2  /*!< 通过 v4l2 接口获取原始帧进行编码（零拷贝） */
    };

    /**
     * @brief 编码帧回调类型
     * @param pData      指向本次编码输出的码流字节，生命周期仅限回调函数内部有效
     * @param size       码流字节数
     * @param isKeyFrame 本帧是否包含关键帧（IDR/I 帧）
     */
    using EncodedFrameCallback = std::function<void(const uint8_t *pData, size_t size, bool isKeyFrame)>;

    /**
     * @brief 外部源帧释放回调（用于 v4l2 零拷贝回收）
     *
     * 回调时机：SDK 触发 source-release（!pStream && pSrc）时。
     * token 由 submitDmabufFd() 调用方传入，通常映射为 v4l2 buffer index。
     */
    using SourceReleaseCallback = std::function<void(uint64_t token)>;

    /**
     * @brief 构造并初始化编码器（包括硬件资源分配）
     * @param cfg      编码参数
     * @param callback 码流输出回调，不可为空
     * @throws std::runtime_error 若硬件初始化或编码器创建失败
     */
    explicit RealtimeEncoder(const EncoderConfig &cfg, EncodedFrameCallback cb, WorkMode mode);

    /**
     * @brief 析构：若尚未调用 flush()，则先触发 EOS 再释放硬件资源
     */
    ~RealtimeEncoder();

    /* 禁止拷贝与移动 */
    RealtimeEncoder(const RealtimeEncoder &) = delete;
    RealtimeEncoder &operator=(const RealtimeEncoder &) = delete;
    RealtimeEncoder(RealtimeEncoder &&) = delete;
    RealtimeEncoder &operator=(RealtimeEncoder &&) = delete;

    /**
     * @brief 从源缓冲池申请一块空闲 DMA 缓冲（阻塞）
     *
     * 配合 submitSourceBuffer() 使用，允许调用方（如 YuvFileSource）直接向
     * DMA 缓冲写入数据，避免二次内存拷贝。
     * GetBuffer 已对返回缓冲执行 AL_Buffer_Ref，调用方填充后须调用
     * submitSourceBuffer() 由其负责 Unref。
     *
     * @return DMA 缓冲指针；编码器已停止或池被 decommit 时返回 nullptr
     */
    AL_TBuffer *acquireSourceBuffer();

    /**
     * @brief 将填充好数据的 DMA 缓冲提交给编码器
     *
     * 调用 AL_Encoder_Process()，SDK 会对缓冲执行内部 Ref，本函数同时
     * Unref 释放用户引用。缓冲由 SDK 持有，编码完毕后经 source-release
     * 回调自动归还缓冲池，无需调用方再次 Unref。
     *
     * @param pBuf 由 acquireSourceBuffer() 获取的缓冲，不可为 nullptr
     * @return true 成功；false 编码器已停止或发生错误
     */
    bool submitSourceBuffer(AL_TBuffer *pBuf);

    /**
     * @brief 提交外部 dmabuf fd 到编码器（零拷贝输入路径）
     *
     * 典型场景：v4l2 DQBUF 获取 dmabuf fd 后直接导入并提交编码器。
     *
     * @param fd      外部 dmabuf 文件描述符
     * @param size    dmabuf 容量（字节）
     * @param token   用户透传标识，source-release 时原样回传
     * @return true   提交成功
     * @return false  导入或提交失败
     */
    bool submitDmabufFd(int fd, size_t size, uint64_t token);

    /**
     * @brief 设置外部源帧释放回调
     */
    void setSourceReleaseCallback(SourceReleaseCallback cb);

    /**
     * @brief 通知编码器输入流结束，等待所有帧编码完成
     *
     * 调用后不可再调用 pushFrame()。
     * 析构函数会自动调用此方法（若未手动调用过）。
     */
    void flush();

    /**
     * @brief 请求编码器将下一帧强制编码为关键帧（IDR）
     *
     * 内部调用 AL_Encoder_RestartGop()，对实时推流中的即时关键帧插入很有用。
     */
    void requestKeyFrame();

    /**
     * @brief 动态修改目标码率（仅对 CBR / VBR 模式有效）
     * @param uTargetBitRate 新的目标码率（bps）
     * @param uMaxBitRate    新的峰值码率（bps），0 = 与目标相同
     * @return true 成功，false 失败（可通过 SDK 日志查看原因）
     */
    bool setBitrate(uint32_t uTargetBitRate, uint32_t uMaxBitRate = 0);

    /* ---- 只读访问（供上层调试/数据源适配使用） ---- */
    TFourCC getSrcFourCC() const
    {
        return m_srcFourCC;
    }
    int getPitchY() const
    {
        return m_pitchY;
    }
    uint8_t getSrcBitDepth() const
    {
        return m_cfg.uBitDepth;
    }
    AL_EChromaMode getChromaMode() const
    {
        return m_cfg.eChromaMode;
    }

  private:
    static void sdkCallback(void *pUserParam, AL_TBuffer *pStream, AL_TBuffer const *pSrc, int iLayerID);
    void onEncodedFrame(AL_TBuffer *pStream, AL_TBuffer const *pSrc);

    void initSettings(AL_TEncSettings &settings) const;
    void initSrcBufPool();
    void initStreamBufPool();
    void pushStreamBuffers();
    void releaseExternSources(AL_TBuffer const *pSrc);

  private:
    const WorkMode m_work_mode;
    /* ---- 配置与回调 ---- */
    EncoderConfig m_cfg;
    EncodedFrameCallback m_callback;

    /* ---- SDK 硬件资源 ---- */
    DMAProxy m_dmaProxy;
    AL_TAllocator *m_pAllocator = nullptr;
    AL_IEncScheduler *m_pScheduler = nullptr;
    AL_HEncoder m_hEnc = nullptr;

    /* ---- 缓冲区池 ----
     * 使用 unique_ptr 管理，确保可以在析构函数中显式控制释放顺序。
     * buffer pool 必须在 allocator 销毁之前释放，因为 BufPool 析构时
     * 会调用 AL_Buffer_Destroy() 释放 DMA 内存。
     */
    std::unique_ptr<PixMapBufPool> m_srcBufPool;
    std::unique_ptr<BufPool> m_streamBufPool;

    /* ---- 编码格式缓存（初始化后不变） ---- */
    AL_TPicFormat m_picFormat{}; /*!< 源图像格式描述 */
    TFourCC m_srcFourCC{};       /*!< 对应 FourCC */
    int m_pitchY = 0;            /*!< 亮度平面行步长（字节） */
    int m_strideH = 0;           /*!< 缓冲区高度步长（字节对齐后的帧高） */

    /* ---- EOS 同步 ---- */
    std::atomic<bool> m_stopped{false};
    std::mutex m_eosMutex;
    std::condition_variable m_eosCv;
    bool m_eosReceived = false;

    /* ---- 错误状态 ---- */
    std::atomic<bool> m_hasError{false};

    struct ExternalSourceContext
    {
        uint64_t token = 0;
    };

    std::mutex m_externalSourcesMutex;
    std::unordered_map<const AL_TBuffer *, ExternalSourceContext> m_externalSources;
    std::mutex m_sourceReleaseCallbackMutex;
    SourceReleaseCallback m_sourceReleaseCallback;
};
