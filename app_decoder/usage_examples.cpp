// 示例 1: 基础使用 - 当前您的方式（已经完全正确）
// ============================================================

#include "RTDecoder.h"
#include "lib_app/YuvIO.h"
#include <fstream>
#include <iostream>

void example_basic_usage() {
    // 配置解码器
    DecoderConfig cfg;
    cfg.codec = AL_CODEC_HEVC;
    cfg.input_mode = AL_DEC_UNSPLIT_INPUT;
    cfg.input_buffer_num = 6;
    cfg.input_buffer_size = 1024 * 1024;
    cfg.timeout_ms = 15000;

    // 打开输出文件
    std::ofstream yuv_file("output.yuv", std::ios::binary);
    if (!yuv_file.is_open()) {
        std::cerr << "Cannot open output file" << std::endl;
        return;
    }

    int frame_count = 0;

    // 创建解码器，注册帧回调
    RTDecoder decoder(cfg, [&](AL_TBuffer *frame, AL_TInfoDecode const &info) {
        // 这个回调在【每一帧】解码完成时被调用
        
        // 1. 应用裁剪信息
        auto *display_meta = reinterpret_cast<AL_TDisplayInfoMetaData *>(
            AL_Buffer_GetMetaData(frame, AL_META_TYPE_DISPLAY_INFO));
        if (display_meta) {
            display_meta->tCrop = info.tCrop;
        }

        // 2. 获取帧尺寸
        auto dim = AL_PixMapBuffer_GetDimension(frame);
        
        // 3. 写入 YUV 文件
        if (!WriteOneFrame(yuv_file, frame)) {
            throw std::runtime_error("Failed to write frame");
        }

        frame_count++;
        if (frame_count % 30 == 0) {  // 每 30 帧打印一次
            std::cout << "Decoded " << frame_count << " frames, "
                      << dim.iWidth << "x" << dim.iHeight << std::endl;
        }
    });

    // 执行解码（阻塞直到完成）
    const bool ok = decoder.decode_file("input.265");
    
    // 解码完成后的处理
    std::cout << "\n=== 解码完成 ===" << std::endl;
    std::cout << "状态: " << (ok ? "成功" : "失败") << std::endl;
    std::cout << "总帧数: " << decoder.decoded_frames() << std::endl;
    std::cout << "损坏帧: " << decoder.concealed_frames() << std::endl;
    
    if (!ok) {
        auto err = decoder.last_error();
        std::cout << "错误: " << AL_Codec_ErrorToString(err) << std::endl;
    }
}


// 示例 2: 提取 HDR 元数据
// ============================================================

extern "C" {
#include "lib_common_dec/HDRMeta.h"
}

void example_extract_hdr() {
    DecoderConfig cfg;
    cfg.codec = AL_CODEC_HEVC;

    std::ofstream hdr_log("hdr_metadata.txt");
    bool hdr_logged = false;  // 只记录一次

    RTDecoder decoder(cfg, [&](AL_TBuffer *frame, AL_TInfoDecode const &info) {
        // 获取 HDR 元数据
        auto* hdr_meta = reinterpret_cast<AL_THDRMetaData*>(
            AL_Buffer_GetMetaData(frame, AL_META_TYPE_HDR));
        
        if (hdr_meta && !hdr_logged) {
            hdr_log << "=== HDR 元数据 ===" << std::endl;
            
            // 色域信息
            hdr_log << "色域类型: " << (int)hdr_meta->eColourDescription << std::endl;
            hdr_log << "传输特性: " << (int)hdr_meta->eTransferCharacteristics << std::endl;
            hdr_log << "色彩矩阵: " << (int)hdr_meta->eColourMatrixCoeffs << std::endl;
            
            // Mastering Display Color Volume (HDR10)
            if (hdr_meta->tHDRSEIs.bHasMDCV) {
                auto& mdcv = hdr_meta->tHDRSEIs.tMDCV;
                hdr_log << "\nMastering Display Info (HDR10):" << std::endl;
                hdr_log << "  红色主色度: (" << mdcv.display_primaries_x[0] 
                        << ", " << mdcv.display_primaries_y[0] << ")" << std::endl;
                hdr_log << "  绿色主色度: (" << mdcv.display_primaries_x[1] 
                        << ", " << mdcv.display_primaries_y[1] << ")" << std::endl;
                hdr_log << "  蓝色主色度: (" << mdcv.display_primaries_x[2] 
                        << ", " << mdcv.display_primaries_y[2] << ")" << std::endl;
                hdr_log << "  白点: (" << mdcv.white_point_x 
                        << ", " << mdcv.white_point_y << ")" << std::endl;
                hdr_log << "  最大亮度: " << mdcv.max_display_mastering_luminance << " nits" << std::endl;
                hdr_log << "  最小亮度: " << mdcv.min_display_mastering_luminance << " nits" << std::endl;
            }
            
            // Content Light Level (HDR10)
            if (hdr_meta->tHDRSEIs.bHasCLL) {
                auto& cll = hdr_meta->tHDRSEIs.tCLL;
                hdr_log << "\nContent Light Level:" << std::endl;
                hdr_log << "  最大内容亮度: " << cll.max_content_light_level << " nits" << std::endl;
                hdr_log << "  最大帧平均亮度: " << cll.max_pic_average_light_level << " nits" << std::endl;
            }
            
            hdr_logged = true;
            std::cout << "HDR 元数据已保存到 hdr_metadata.txt" << std::endl;
        }
        
        // 继续处理帧...
    });

    decoder.decode_file("hdr_video.265");
}


// 示例 3: 统计和分析
// ============================================================

void example_statistics() {
    DecoderConfig cfg;
    cfg.codec = AL_CODEC_HEVC;

    struct Statistics {
        int total_frames = 0;
        int error_frames = 0;
        std::map<std::string, int> resolution_count;
        std::chrono::steady_clock::time_point start_time;
    } stats;

    stats.start_time = std::chrono::steady_clock::now();

    RTDecoder decoder(cfg, [&](AL_TBuffer *frame, AL_TInfoDecode const &info) {
        stats.total_frames++;

        // 统计分辨率分布
        auto dim = AL_PixMapBuffer_GetDimension(frame);
        std::string res = std::to_string(dim.iWidth) + "x" + std::to_string(dim.iHeight);
        stats.resolution_count[res]++;

        // 检查帧错误（通过元数据）
        auto* pic_meta = reinterpret_cast<AL_TPictureDecMetaData*>(
            AL_Buffer_GetMetaData(frame, AL_META_TYPE_PICTURE_DEC));
        if (pic_meta && pic_meta->bDecFailed) {
            stats.error_frames++;
        }

        // 实时显示进度
        if (stats.total_frames % 100 == 0) {
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - stats.start_time).count();
            double fps = stats.total_frames / (double)elapsed;
            std::cout << "\r解码进度: " << stats.total_frames << " 帧 "
                      << "(" << std::fixed << std::setprecision(1) << fps << " fps)" << std::flush;
        }
    });

    bool ok = decoder.decode_file("input.265");

    // 打印统计信息
    auto end_time = std::chrono::steady_clock::now();
    auto total_seconds = std::chrono::duration_cast<std::chrono::seconds>(end_time - stats.start_time).count();

    std::cout << "\n\n=== 解码统计 ===" << std::endl;
    std::cout << "总时长: " << total_seconds << " 秒" << std::endl;
    std::cout << "总帧数: " << stats.total_frames << std::endl;
    std::cout << "平均帧率: " << (stats.total_frames / (double)total_seconds) << " fps" << std::endl;
    std::cout << "损坏帧: " << decoder.concealed_frames() << std::endl;
    std::cout << "错误帧: " << stats.error_frames << std::endl;
    
    std::cout << "\n分辨率分布:" << std::endl;
    for (const auto& [res, count] : stats.resolution_count) {
        std::cout << "  " << res << ": " << count << " 帧" << std::endl;
    }
}


// 示例 4: 多个输出目标
// ============================================================

void example_multiple_outputs() {
    DecoderConfig cfg;
    cfg.codec = AL_CODEC_HEVC;

    // 准备多个输出
    std::ofstream yuv_file("output.yuv", std::ios::binary);
    std::ofstream metadata_file("metadata.txt");
    std::vector<std::vector<uint8_t>> frames_buffer;  // 存储部分帧到内存

    RTDecoder decoder(cfg, [&](AL_TBuffer *frame, AL_TInfoDecode const &info) {
        auto dim = AL_PixMapBuffer_GetDimension(frame);

        // 输出 1: 写入 YUV 文件
        WriteOneFrame(yuv_file, frame);

        // 输出 2: 记录元数据
        metadata_file << "Frame " << frames_buffer.size() << ": "
                     << dim.iWidth << "x" << dim.iHeight
                     << ", Crop: (" << info.tCrop.uCropOffsetLeft << ", "
                     << info.tCrop.uCropOffsetTop << ")" << std::endl;

        // 输出 3: 每 10 帧存储一帧到内存（例如生成缩略图）
        if (frames_buffer.size() % 10 == 0) {
            // 注意：这里需要拷贝数据，因为 frame 缓冲区会被重用
            size_t frame_size = AL_Buffer_GetSize(frame);
            std::vector<uint8_t> frame_copy(frame_size);
            memcpy(frame_copy.data(), AL_Buffer_GetData(frame), frame_size);
            frames_buffer.push_back(std::move(frame_copy));
        }
    });

    decoder.decode_file("input.265");

    std::cout << "已保存 " << frames_buffer.size() << " 个关键帧到内存" << std::endl;
}


// 示例 5: 错误处理和恢复
// ============================================================

void example_error_handling() {
    DecoderConfig cfg;
    cfg.codec = AL_CODEC_HEVC;
    cfg.timeout_ms = 10000;  // 10 秒超时

    int successful_frames = 0;
    int failed_frames = 0;

    try {
        RTDecoder decoder(cfg, [&](AL_TBuffer *frame, AL_TInfoDecode const &info) {
            try {
                // 尝试处理帧
                // 如果处理失败，抛出异常
                
                successful_frames++;
            } catch (const std::exception& e) {
                failed_frames++;
                std::cerr << "处理第 " << (successful_frames + failed_frames) 
                         << " 帧时出错: " << e.what() << std::endl;
                // 继续处理下一帧
            }
        });

        bool ok = decoder.decode_file("input.265");

        if (!ok) {
            auto err = decoder.last_error();
            std::cerr << "解码失败: " << AL_Codec_ErrorToString(err) << std::endl;
            
            // 根据错误类型采取不同措施
            if (err == AL_ERR_NO_MEMORY) {
                std::cerr << "内存不足，尝试减少缓冲区数量" << std::endl;
            } else if (err == AL_ERR_STREAM_OVERFLOW) {
                std::cerr << "码流错误，可能文件损坏" << std::endl;
            }
        }

    } catch (const std::exception& e) {
        std::cerr << "解码器异常: " << e.what() << std::endl;
    }

    std::cout << "成功: " << successful_frames << " 帧" << std::endl;
    std::cout << "失败: " << failed_frames << " 帧" << std::endl;
}


// 示例 6: 与现有代码集成
// ============================================================

class VideoProcessor {
public:
    bool process_video(const std::string& input_path) {
        DecoderConfig cfg;
        cfg.codec = detect_codec(input_path);
        
        try {
            RTDecoder decoder(cfg, [this](AL_TBuffer *frame, AL_TInfoDecode const &info) {
                this->on_frame_decoded(frame, info);
            });
            
            return decoder.decode_file(input_path);
        } catch (const std::exception& e) {
            std::cerr << "视频处理失败: " << e.what() << std::endl;
            return false;
        }
    }

private:
    void on_frame_decoded(AL_TBuffer *frame, AL_TInfoDecode const &info) {
        // 在类方法中处理解码帧
        // 可以访问类成员变量
        // 可以调用其他处理函数
        process_frame_data(frame);
        update_statistics(info);
    }

    void process_frame_data(AL_TBuffer *frame) {
        // 您的帧处理逻辑
    }

    void update_statistics(AL_TInfoDecode const &info) {
        // 更新统计信息
    }

    AL_ECodec detect_codec(const std::string& path) {
        if (path.ends_with(".264")) return AL_CODEC_AVC;
        return AL_CODEC_HEVC;
    }
};


// 主函数示例
// ============================================================

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "用法: " << argv[0] << " <示例编号>" << std::endl;
        std::cerr << "  1: 基础使用" << std::endl;
        std::cerr << "  2: 提取 HDR 元数据" << std::endl;
        std::cerr << "  3: 统计和分析" << std::endl;
        std::cerr << "  4: 多个输出目标" << std::endl;
        std::cerr << "  5: 错误处理" << std::endl;
        return 1;
    }

    int example_num = std::atoi(argv[1]);

    switch (example_num) {
        case 1: example_basic_usage(); break;
        case 2: example_extract_hdr(); break;
        case 3: example_statistics(); break;
        case 4: example_multiple_outputs(); break;
        case 5: example_error_handling(); break;
        default:
            std::cerr << "未知的示例编号: " << example_num << std::endl;
            return 1;
    }

    return 0;
}
