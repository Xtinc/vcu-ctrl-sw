#include "RTDecoder.h"

#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

extern "C"
{
#include "lib_common/DisplayInfoMeta.h"
#include "lib_common/PixMapBuffer.h"
}

#include "lib_app/YuvIO.h"

namespace
{
void PrintUsage(const char *app)
{
    std::cerr << "Usage: " << app << " <input.264|input.265> <output.yuv> [avc|hevc] [--split-sizes sizes.txt]" << std::endl;
}

AL_ECodec ParseCodec(const std::string &codec_text, const std::string &input_path)
{
    if (!codec_text.empty())
    {
        if (codec_text == "avc")
        {
            return AL_CODEC_AVC;
        }

        if (codec_text == "hevc")
        {
            return AL_CODEC_HEVC;
        }

        throw std::runtime_error("Unsupported codec argument: " + codec_text);
    }

    if (input_path.size() >= 4 && input_path.substr(input_path.size() - 4) == ".264")
    {
        return AL_CODEC_AVC;
    }

    return AL_CODEC_HEVC;
}
}

int main(int argc, char **argv)
{
    try
    {
        if (argc < 3)
        {
            PrintUsage(argv[0]);
            return 1;
        }

        const std::string input_path = argv[1];
        const std::string output_path = argv[2];
        std::string codec_arg;
        std::string split_sizes_path;

        for (int i = 3; i < argc; ++i)
        {
            const std::string arg = argv[i];

            if (arg == "avc" || arg == "hevc")
            {
                if (!codec_arg.empty())
                {
                    throw std::runtime_error("Duplicated codec argument");
                }
                codec_arg = arg;
                continue;
            }

            if (arg == "--split-sizes")
            {
                if (i + 1 >= argc)
                {
                    throw std::runtime_error("--split-sizes requires a file path");
                }

                split_sizes_path = argv[++i];
                continue;
            }

            throw std::runtime_error("Unknown argument: " + arg);
        }

        std::ofstream output_file(output_path, std::ios::binary);
        if (!output_file.is_open())
        {
            throw std::runtime_error("Cannot open output file: " + output_path);
        }

        DecoderConfig cfg;
        cfg.codec = ParseCodec(codec_arg, input_path);
        cfg.input_mode = split_sizes_path.empty() ? AL_DEC_UNSPLIT_INPUT : AL_DEC_SPLIT_INPUT;
        cfg.input_buffer_num = 6;
        cfg.input_buffer_size = 1024 * 1024;
        cfg.timeout_ms = 15000;

        AL_TDimension last_dim { 0, 0 };

        RTDecoder decoder(cfg, [&](AL_TBuffer *frame, AL_TInfoDecode const &info) {
            // Note: RTDecoder guarantees this callback only receives main/postproc frames
            
            // Apply crop information to display metadata
            auto *display_meta = reinterpret_cast<AL_TDisplayInfoMetaData *>(
                AL_Buffer_GetMetaData(frame, AL_META_TYPE_DISPLAY_INFO));
            if (display_meta)
            {
                display_meta->tCrop = info.tCrop;
            }

            auto dim = AL_PixMapBuffer_GetDimension(frame);
            if (dim.iWidth != last_dim.iWidth || dim.iHeight != last_dim.iHeight)
            {
                std::cout << "Decoded resolution: " << dim.iWidth << "x" << dim.iHeight << std::endl;
                last_dim = dim;
            }

            if (!WriteOneFrame(output_file, frame))
            {
                throw std::runtime_error("WriteOneFrame failed");
            }
        });

        const bool ok = decoder.decode_file(input_path, split_sizes_path);
        std::cout << "Decode done, frames=" << decoder.decoded_frames()
                  << ", concealed=" << decoder.concealed_frames()
                  << ", status=" << (ok ? "OK" : "ERROR") << std::endl;

        return ok ? 0 : 2;
    }
    catch (std::exception const &e)
    {
        std::cerr << "Exception: " << e.what() << std::endl;
        return 1;
    }
}
