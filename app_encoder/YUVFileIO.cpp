#include "YUVFileIO.h"

extern "C"
{
#include "lib_common/BufCommon.h"
#include "lib_common/PixMapBuffer.h"
#include "lib_rtos/message.h"
}

YuvFileIO::YuvFileIO(std::string filePath, Mode mode, int width, int height, TFourCC fourCC, bool loopRead)
    : m_path(std::move(filePath)), m_mode(mode), m_width(width), m_height(height), m_fourCC(fourCC), m_loop(loopRead)
{
}

bool YuvFileIO::open()
{
    if (!validate_configuration())
    {
        return false;
    }
    if (m_mode == Mode::Read)
    {
        m_ifs.open(m_path, std::ios::in | std::ios::binary);
        if (!m_ifs.is_open())
        {
            VIDEO_ERROR_PRINT("[YuvFileIO] failed to open input file: %s\n", m_path.c_str());
            return false;
        }
    }
    else
    {
        m_ofs.open(m_path, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!m_ofs.is_open())
        {
            VIDEO_ERROR_PRINT("[YuvFileIO] failed to open output file: %s\n", m_path.c_str());
            return false;
        }
    }

    return true;
}

void YuvFileIO::close()
{
    if (m_ifs.is_open())
    {
        m_ifs.close();
    }

    if (m_ofs.is_open())
    {
        m_ofs.close();
    }
}

bool YuvFileIO::is_open() const
{
    return m_mode == Mode::Read ? m_ifs.is_open() : m_ofs.is_open();
}

bool YuvFileIO::read_frame(AL_TBuffer *pBuf)
{
    if (m_mode != Mode::Read)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] file was not opened in read mode\n");
        return false;
    }

    if (!m_ifs.is_open())
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] input file is not opened\n");
        return false;
    }

    if (!validate_buffer(pBuf))
    {
        return false;
    }

    if (m_ifs.peek() == EOF)
    {
        if (!m_loop)
        {
            return false;
        }
        m_ifs.clear();
        m_ifs.seekg(0, std::ios::beg);
    }

    AL_TPicFormat tPicFormat;
    if (!AL_GetPicFormat(m_fourCC, &tPicFormat))
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid fourcc\n");
        return false;
    }
    auto ok = false;
    if (tPicFormat.eChromaMode == AL_CHROMA_4_0_0)
    {
        ok = access_mono_frame(pBuf, true);
    }
    else if (tPicFormat.ePlaneMode == AL_PLANE_MODE_SEMIPLANAR)
    {
        ok = access_semi_planar_frame(pBuf, true);
    }
    else if (tPicFormat.ePlaneMode == AL_PLANE_MODE_PLANAR)
    {
        ok = access_planar_frame(pBuf, true);
    }
    else
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] unsupported plane mode\n");
    }

    return ok;
}

bool YuvFileIO::write_frame(AL_TBuffer const *pBuf)
{
    if (m_mode != Mode::Write)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] file was not opened in write mode\n");
        return false;
    }

    if (!m_ofs.is_open())
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] output file is not opened\n");
        return false;
    }

    if (!validate_buffer(pBuf))
    {
        return false;
    }

    AL_TPicFormat tPicFormat;
    if (!AL_GetPicFormat(m_fourCC, &tPicFormat))
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid fourcc\n");
        return false;
    }

    auto ok = false;
    auto editableBuf = const_cast<AL_TBuffer *>(pBuf);
    if (tPicFormat.eChromaMode == AL_CHROMA_4_0_0)
    {
        ok = access_mono_frame(editableBuf, false);
    }
    else if (tPicFormat.ePlaneMode == AL_PLANE_MODE_SEMIPLANAR)
    {
        ok = access_semi_planar_frame(editableBuf, false);
    }
    else if (tPicFormat.ePlaneMode == AL_PLANE_MODE_PLANAR)
    {
        ok = access_planar_frame(editableBuf, false);
    }
    else
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] unsupported plane mode\n");
    }
    return ok;
}

bool YuvFileIO::seek_frame(size_t frameIndex)
{
    size_t frameSize = get_frame_size();
    if (frameSize == 0)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid frame size\n");
        return false;
    }

    if (m_mode == Mode::Read)
    {
        if (!m_ifs.is_open())
        {
            VIDEO_ERROR_PRINT("[YuvFileIO] input file is not opened\n");
            return false;
        }
        m_ifs.clear();
        m_ifs.seekg(frameIndex * frameSize, std::ios::beg);
        return m_ifs.good();
    }
    else
    {
        if (!m_ofs.is_open())
        {
            VIDEO_ERROR_PRINT("[YuvFileIO] output file is not opened\n");
            return false;
        }
        m_ofs.clear();
        m_ofs.seekp(frameIndex * frameSize, std::ios::beg);
        return m_ofs.good();
    }
    return false;
}

size_t YuvFileIO::tell_frame()
{
    size_t frameSize = get_frame_size();

    if (frameSize == 0)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid frame size\n");
        return 0;
    }

    if (m_mode == Mode::Read)
    {
        if (!m_ifs.is_open())
        {
            VIDEO_ERROR_PRINT("[YuvFileIO] input file is not opened\n");
            return 0;
        }
        auto pos = m_ifs.tellg();
        if (pos < 0)
        {
            VIDEO_ERROR_PRINT("[YuvFileIO] failed to tell input file position\n");
            return 0;
        }
        return static_cast<size_t>(pos) / frameSize;
    }
    else
    {
        if (!m_ofs.is_open())
        {
            VIDEO_ERROR_PRINT("[YuvFileIO] output file is not opened\n");
            return 0;
        }
        auto pos = m_ofs.tellp();
        if (pos < 0)
        {
            VIDEO_ERROR_PRINT("[YuvFileIO] failed to tell output file position\n");
            return 0;
        }
        return static_cast<size_t>(pos) / frameSize;
    }
}

size_t YuvFileIO::get_frame_size() const
{
    if (m_width <= 0 || m_height <= 0)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid frame dimension\n");
        return 0;
    }

    auto lumaRowSize = m_width;
    auto lumaRows = m_height;

    auto chromaRowSize = AL_GetChromaPitch(m_fourCC, lumaRowSize);
    auto chromaRows = AL_GetChromaHeight(m_fourCC, lumaRows);

    if (chromaRowSize < 0 || chromaRows < 0)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid chroma geometry\n");
        return 0;
    }

    AL_TPicFormat picFormat;
    if (!AL_GetPicFormat(m_fourCC, &picFormat))
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid fourcc\n");
        return 0;
    }

    size_t chromaSize = static_cast<size_t>(chromaRowSize) * static_cast<size_t>(chromaRows);

    if (picFormat.eChromaMode == AL_CHROMA_4_0_0)
    {
        return lumaRowSize * lumaRows;
    }
    else if (picFormat.ePlaneMode == AL_PLANE_MODE_SEMIPLANAR)
    {
        return lumaRowSize * lumaRows + chromaSize;
    }
    else if (picFormat.ePlaneMode == AL_PLANE_MODE_PLANAR)
    {
        return lumaRowSize * lumaRows + 2 * chromaSize;
    }
    else
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] unsupported plane mode\n");
    }

    return 0;
}

bool YuvFileIO::supported(TFourCC fourCC)
{
    AL_TPicFormat picFormat;
    if (!AL_GetPicFormat(fourCC, &picFormat))
    {
        return false;
    }

    if (picFormat.bCompressed)
    {
        return false;
    }

    if (picFormat.eStorageMode != AL_FB_RASTER)
    {
        return false;
    }

    if (picFormat.uBitDepth != 8)
    {
        return false;
    }

    if (picFormat.ePlaneMode != AL_PLANE_MODE_PLANAR && picFormat.ePlaneMode != AL_PLANE_MODE_SEMIPLANAR)
    {
        return false;
    }

    return true;
}

bool YuvFileIO::validate_configuration()
{
    if (m_width <= 0 || m_height <= 0)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid frame dimension\n");
        return false;
    }

    if (!supported(m_fourCC))
    {
        VIDEO_ERROR_PRINT(
            "[YuvFileIO] unsupported format: expected 8-bit raster planar/semi-planar (e.g. NV12/NV16)\n");
        return false;
    }

    return true;
}

bool YuvFileIO::validate_buffer(AL_TBuffer const *pBuf)
{
    if (!pBuf)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] null AL_TBuffer\n");
        return false;
    }

    AL_TDimension dim = AL_PixMapBuffer_GetDimension(pBuf);
    if (dim.iWidth != static_cast<int32_t>(m_width) || dim.iHeight != static_cast<int32_t>(m_height))
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] buffer dimension does not match file configuration\n");
        return false;
    }

    if (AL_PixMapBuffer_GetFourCC(pBuf) != m_fourCC)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] buffer format does not match file configuration\n");
        return false;
    }

    AL_TPicFormat picFormat;
    if (!AL_GetPicFormat(m_fourCC, &picFormat))
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid fourcc\n");
        return false;
    }

    auto lumaPitch = AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_Y);
    if (lumaPitch < static_cast<int>(m_width))
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] buffer luma pitch is smaller than frame width\n");
        return false;
    }

    return true;
}

bool YuvFileIO::read_plane(uint8_t *pDst, int iPitch, int iRowSize, int iNumRows)
{
    if (!pDst || iPitch < iRowSize || iRowSize <= 0 || iNumRows <= 0)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid read plane parameters\n");
        return false;
    }

    for (int h = 0; h < iNumRows; ++h)
    {
        m_ifs.read(reinterpret_cast<char *>(pDst), iRowSize);
        if (m_ifs.gcount() != iRowSize)
        {
            VIDEO_ERROR_PRINT("[YuvFileIO] not enough data for one complete frame\n");
            return false;
        }
        pDst += iPitch;
    }

    return true;
}

bool YuvFileIO::write_plane(uint8_t const *pSrc, int iPitch, int iRowSize, int iNumRows)
{
    if (!pSrc || iPitch < iRowSize || iRowSize <= 0 || iNumRows <= 0)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid write plane parameters\n");
        return false;
    }

    for (int h = 0; h < iNumRows; ++h)
    {
        m_ofs.write(reinterpret_cast<char const *>(pSrc), iRowSize);
        if (!m_ofs.good())
        {
            VIDEO_ERROR_PRINT("[YuvFileIO] failed to write one complete frame\n");
            return false;
        }
        pSrc += iPitch;
    }

    return true;
}

bool YuvFileIO::access_planar_frame(AL_TBuffer *pBuf, bool bReadMode)
{
    auto lumaRowSize = m_width;
    auto lumaRows = m_height;
    auto chromaRowSize = AL_GetChromaPitch(m_fourCC, lumaRowSize);
    auto chromaRows = AL_GetChromaHeight(m_fourCC, lumaRows);

    if (chromaRowSize <= 0 || chromaRows <= 0)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid planar chroma geometry\n");
        return false;
    }

    auto pY = AL_PixMapBuffer_GetPlaneAddress(pBuf, AL_PLANE_Y);
    auto pU = AL_PixMapBuffer_GetPlaneAddress(pBuf, AL_PLANE_U);
    auto pV = AL_PixMapBuffer_GetPlaneAddress(pBuf, AL_PLANE_V);
    auto iPitchY = AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_Y);
    auto iPitchU = AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_U);
    auto iPitchV = AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_V);

    if (!pY || !pU || !pV)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] missing planar addresses in buffer\n");
        return false;
    }

    AL_TPicFormat tPicFormat;
    if (!AL_GetPicFormat(m_fourCC, &tPicFormat))
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid fourcc\n");
        return false;
    }

    auto pC1 = tPicFormat.eComponentOrder == AL_COMPONENT_ORDER_YVU ? pV : pU;
    auto pC2 = tPicFormat.eComponentOrder == AL_COMPONENT_ORDER_YVU ? pU : pV;
    auto iPitchC1 = tPicFormat.eComponentOrder == AL_COMPONENT_ORDER_YVU ? iPitchV : iPitchU;
    auto iPitchC2 = tPicFormat.eComponentOrder == AL_COMPONENT_ORDER_YVU ? iPitchU : iPitchV;
    if (bReadMode)
    {
        return read_plane(pY, iPitchY, lumaRowSize, lumaRows) && read_plane(pC1, iPitchC1, chromaRowSize, chromaRows) &&
               read_plane(pC2, iPitchC2, chromaRowSize, chromaRows);
    }
    else
    {
        return write_plane(pY, iPitchY, lumaRowSize, lumaRows) &&
               write_plane(pC1, iPitchC1, chromaRowSize, chromaRows) &&
               write_plane(pC2, iPitchC2, chromaRowSize, chromaRows);
    }
}

bool YuvFileIO::access_semi_planar_frame(AL_TBuffer *pBuf, bool bReadMode)
{
    auto lumaRowSize = m_width;
    auto lumaRows = m_height;
    auto chromaRowSize = AL_GetChromaPitch(m_fourCC, lumaRowSize);
    auto chromaRows = AL_GetChromaHeight(m_fourCC, lumaRows);

    if (chromaRowSize <= 0 || chromaRows <= 0)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] invalid semi-planar chroma geometry\n");
        return false;
    }

    auto pY = AL_PixMapBuffer_GetPlaneAddress(pBuf, AL_PLANE_Y);
    auto pUV = AL_PixMapBuffer_GetPlaneAddress(pBuf, AL_PLANE_UV);
    auto iPitchY = AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_Y);
    auto iPitchUV = AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_UV);

    if (!pY || !pUV)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] missing semiplanar addresses in buffer\n");
        return false;
    }

    if (bReadMode)
    {
        return read_plane(pY, iPitchY, lumaRowSize, lumaRows) && read_plane(pUV, iPitchUV, chromaRowSize, chromaRows);
    }
    else
    {
        return write_plane(pY, iPitchY, lumaRowSize, lumaRows) && write_plane(pUV, iPitchUV, chromaRowSize, chromaRows);
    }
}

bool YuvFileIO::access_mono_frame(AL_TBuffer *pBuf, bool bReadMode)
{
    auto lumaRowSize = m_width;
    auto lumaRows = m_height;

    auto pY = AL_PixMapBuffer_GetPlaneAddress(pBuf, AL_PLANE_Y);
    auto iPitchY = AL_PixMapBuffer_GetPlanePitch(pBuf, AL_PLANE_Y);

    if (!pY)
    {
        VIDEO_ERROR_PRINT("[YuvFileIO] missing mono address in buffer\n");
        return false;
    }

    if (bReadMode)
    {
        return read_plane(pY, iPitchY, lumaRowSize, lumaRows);
    }
    else
    {
        return write_plane(pY, iPitchY, lumaRowSize, lumaRows);
    }
}
