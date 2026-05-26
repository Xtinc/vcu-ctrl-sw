#include "RTEncoder.h"
#include "YUVFileIO.h"

extern "C"
{
#include "lib_common/FourCC.h"
#include "lib_rtos/message.h"
}

#include <fstream>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace
{
static constexpr unsigned int kStatLogInterval = 50;

struct FileEncodeCmd
{
    std::string input_path;
    uint16_t width = 0;
    uint16_t height = 0;
    unsigned int num_frames = 0;
};

void print_usage(const char *app)
{
    VIDEO_ERROR_PRINT("Usage: %s <cmd.txt> <output.265>", app);
}

EncoderConfig make_default_config()
{
    EncoderConfig cfg{};
    cfg.rc_mode = AL_RC_CBR;
    cfg.target_bitrate = 8000000;
    cfg.framerate = 60;
    cfg.clk_ratio = 1000;
    cfg.gop_length = 30;
    cfg.num_b = 0;
    cfg.low_delay_mode = true;
    return cfg;
}

bool parse_u16(const std::string &token, uint16_t &value)
{
    try
    {
        const auto raw = std::stoul(token);
        if (raw == 0 || raw > std::numeric_limits<uint16_t>::max())
        {
            return false;
        }
        value = static_cast<uint16_t>(raw);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool parse_uint(const std::string &token, unsigned int &value)
{
    try
    {
        const auto raw = std::stoul(token);
        if (raw > std::numeric_limits<unsigned int>::max())
        {
            return false;
        }
        value = static_cast<unsigned int>(raw);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

bool parse_file_cmds(const std::string &cmd_file_path, std::vector<FileEncodeCmd> &cmds, TFourCC &fourcc)
{
    std::ifstream cmd_file(cmd_file_path);
    if (!cmd_file)
    {
        VIDEO_ERROR_PRINT("Cannot open command file: %s", cmd_file_path.c_str());
        return false;
    }

    cmds.clear();
    fourcc = FOURCC(NV12);

    std::string line;
    size_t line_no = 0;
    while (std::getline(cmd_file, line))
    {
        ++line_no;
        std::istringstream iss(line);
        std::vector<std::string> tokens;
        for (std::string token; iss >> token;)
        {
            tokens.push_back(token);
        }

        if (tokens.empty())
        {
            continue;
        }

        if (tokens.size() < 4)
        {
            VIDEO_ERROR_PRINT("Invalid command line at %zu: need at least 4 fields", line_no);
            return false;
        }

        FileEncodeCmd cmd;
        cmd.input_path = tokens[0];
        if (!parse_u16(tokens[1], cmd.width) || !parse_u16(tokens[2], cmd.height) ||
            !parse_uint(tokens[3], cmd.num_frames))
        {
            VIDEO_ERROR_PRINT("Invalid numeric field at line %zu", line_no);
            return false;
        }

        if (tokens.size() >= 5)
        {
            const auto line_fourcc = STR2FOURCC(tokens[4]);
            if (line_fourcc == 0)
            {
                VIDEO_ERROR_PRINT("Invalid FOURCC at line %zu", line_no);
                return false;
            }

            if (cmds.empty())
            {
                fourcc = line_fourcc;
            }
            else if (line_fourcc != fourcc)
            {
                VIDEO_ERROR_PRINT("FourCC mismatch at line %zu", line_no);
                return false;
            }
        }

        cmds.push_back(cmd);
    }

    if (cmds.empty())
    {
        VIDEO_ERROR_PRINT("No valid command lines in %s", cmd_file_path.c_str());
        return false;
    }

    return true;
}
} // namespace

int main(int argc, char *argv[])
{
    message_init();
    if (argc != 3)
    {
        print_usage(argv[0]);
        return EXIT_FAILURE;
    }

    const std::string cmdFilePath = argv[1];
    std::ofstream outFile(argv[2], std::ios::binary);
    if (!outFile)
    {
        VIDEO_ERROR_PRINT("Cannot open output file: %s", argv[2]);
        return EXIT_FAILURE;
    }

    std::vector<FileEncodeCmd> cmds;
    TFourCC fourcc = FOURCC(NV12);
    if (!parse_file_cmds(cmdFilePath, cmds, fourcc))
    {
        return EXIT_FAILURE;
    }

    EncoderConfig cfg = make_default_config();
    unsigned int totalEncodedUnits = 0;

    try
    {
        cfg.width = cmds.front().width;
        cfg.height = cmds.front().height;
        cfg.chroma_mode = (fourcc == FOURCC(NV12)) ? AL_CHROMA_4_2_0 : AL_CHROMA_4_2_2;

        auto encoder = std::make_unique<RTEncoderFile>(cfg, [&](const uint8_t *pData, size_t size, bool) {
            VIDEO_INFO_PRINT("[%6u] size: %6zu bytes", totalEncodedUnits, size);
            outFile.write(reinterpret_cast<const char *>(pData), size);
            ++totalEncodedUnits;
        });

        uint16_t current_width = cfg.width;
        uint16_t current_height = cfg.height;
        for (size_t cmdIdx = 0; cmdIdx < cmds.size(); ++cmdIdx)
        {
            const auto &cmd = cmds[cmdIdx];

            YuvFileIO yuvFile(cmd.input_path, YuvFileIO::Mode::Read, cmd.width, cmd.height, fourcc);
            if (!yuvFile.open())
            {
                VIDEO_ERROR_PRINT("Failed to open YUV file: %s", cmd.input_path.c_str());
                continue;
            }

            if (current_width != cmd.width || current_height != cmd.height)
            {
                if (!encoder->set_resolution(cmd.width, cmd.height))
                {
                    VIDEO_ERROR_PRINT("Failed to apply resolution change at line %zu: %ux%u", cmdIdx + 1, cmd.width,
                                      cmd.height);
                    return EXIT_FAILURE;
                }

                encoder->request_IDR();
                current_width = cmd.width;
                current_height = cmd.height;
                VIDEO_INFO_PRINT("Resolution change at line %zu: %ux%u", cmdIdx + 1, cmd.width, cmd.height);
            }

            VIDEO_INFO_PRINT("Start encoding %u frames from %s", cmd.num_frames, cmd.input_path.c_str());
            for (unsigned int i = 0; i < cmd.num_frames; i++)
            {
                AL_TBuffer *srcBuf = encoder->acquire_source_buffer();
                if (!srcBuf)
                {
                    VIDEO_ERROR_PRINT("Failed to acquire source buffer at frame %u", i);
                    break;
                }
                if (!yuvFile.read_frame(srcBuf))
                {
                    AL_Buffer_Unref(srcBuf);
                    break;
                }
                if (!encoder->submit_source_buffer(srcBuf))
                {
                    VIDEO_ERROR_PRINT("Failed to submit source buffer at frame %u", i);
                    return EXIT_FAILURE;
                }

                if (i % kStatLogInterval == 0)
                {
                    auto p = encoder->fps();
                    VIDEO_INFO_PRINT("FPS: %.2f, BitRate:%.2f kbps", p.first, p.second / 1000.0);
                }
            }
        }

        if (!encoder->flush())
        {
            VIDEO_ERROR_PRINT("Encoder flush timed out; output may be incomplete");
        }
        VIDEO_INFO_PRINT("All tasks done. Total encoded units: %u", totalEncodedUnits);
        return EXIT_SUCCESS;
    }
    catch (const std::exception &e)
    {
        VIDEO_ERROR_PRINT("Encoder error: %s", e.what());
        return EXIT_FAILURE;
    }
}
