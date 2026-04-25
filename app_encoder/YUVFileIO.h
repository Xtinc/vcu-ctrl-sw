#ifndef VIDEO_YUVFILEIO_H
#define VIDEO_YUVFILEIO_H

extern "C"
{
#include "lib_common/BufferAPI.h"
#include "lib_common/FourCC.h"
#include "lib_rtos/message.h"
}

#include <cstddef>
#include <fstream>
#include <string>
#include <utility>

class YuvFileIO
{
  public:
    enum class Mode
    {
        Read,
        Write,
    };

    YuvFileIO(std::string filePath, Mode mode, int width, int height, TFourCC fourCC, bool loopRead = false);

    bool open();
    void close();
    bool is_open() const;

    bool read_frame(AL_TBuffer* pBuf);
    bool write_frame(AL_TBuffer const* pBuf);
    bool seek_frame(size_t frameIndex);
    size_t tell_frame();
    size_t get_frame_size() const;

    static bool supported(TFourCC fourCC);

    private:
    bool validate_configuration();
    bool validate_buffer(AL_TBuffer const* pBuf);
    bool read_plane(uint8_t* pDst, int iPitch, int iRowSize, int iNumRows);
    bool write_plane(uint8_t const* pSrc, int iPitch, int iRowSize, int iNumRows);
    bool access_planar_frame(AL_TBuffer* pBuf, bool bReadMode);
    bool access_semi_planar_frame(AL_TBuffer* pBuf, bool bReadMode);
    bool access_mono_frame(AL_TBuffer* pBuf, bool bReadMode);

    private:
    std::string m_path;
    Mode m_mode;
    size_t m_width;
    size_t m_height;
    TFourCC m_fourCC;
    bool m_loop;

    std::ifstream m_ifs;
    std::ofstream m_ofs;
};

#endif // YUVFILEIO_H