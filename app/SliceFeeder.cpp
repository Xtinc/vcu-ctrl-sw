#include "SliceFeeder.h"

extern "C"
{
#include "lib_common/AvcUtils.h"
#include "lib_common/HevcUtils.h"
#include "lib_common/Nuts.h"
#include "lib_decode/lib_decode.h"
#include "lib_rtos/message.h"
}

struct StartCodeMatch
{
    size_t offset = 0;
    size_t size = 0;
    bool found = false;
};

static StartCodeMatch find_start_code(std::vector<uint8_t> const &buf, size_t from)
{
    StartCodeMatch m;
    if (buf.size() < 3 || from >= buf.size())
    {
        return m;
    }

    for (size_t i = from; (i + 2) < buf.size(); ++i)
    {
        if (buf[i] != 0x00 || buf[i + 1] != 0x00)
        {
            continue;
        }

        if (buf[i + 2] == 0x01)
        {
            m.offset = i;
            m.size = 3;
            m.found = true;
            return m;
        }

        if ((i + 3) < buf.size() && buf[i + 2] == 0x00 && buf[i + 3] == 0x01)
        {
            m.offset = i;
            m.size = 4;
            m.found = true;
            return m;
        }
    }

    return m;
}

struct NalInfo
{
    bool valid = false;
    bool is_vcl = false;
    bool is_first_slice = false;
};

static NalInfo parse_nal_info(AL_ECodec codec, std::vector<uint8_t> const &buf, StartCodeMatch const &sc,
                              size_t nal_end)
{
    NalInfo info;

    const size_t nal_hdr = sc.offset + sc.size;
    if (nal_hdr >= nal_end || nal_hdr >= buf.size())
    {
        return info;
    }

    if (codec == AL_CODEC_AVC)
    {
        AL_ENut nut = static_cast<AL_ENut>(buf[nal_hdr] & 0x1F);
        info.is_vcl = AL_AVC_IsVcl(nut);
        info.valid = true;
        if (info.is_vcl)
        {
            const size_t first_mb = nal_hdr + 1;
            info.is_first_slice = (first_mb < nal_end) && ((buf[first_mb] & 0x80) != 0);
        }
        return info;
    }

    if (codec == AL_CODEC_HEVC)
    {
        if ((nal_hdr + 1) >= nal_end || (nal_hdr + 1) >= buf.size())
        {
            return info;
        }

        AL_ENut nut = static_cast<AL_ENut>((buf[nal_hdr] >> 1) & 0x3F);
        info.is_vcl = AL_HEVC_IsVcl(nut);
        info.valid = true;
        if (info.is_vcl)
        {
            const size_t first_slice_seg = nal_hdr + 2;
            info.is_first_slice = (first_slice_seg < nal_end) && ((buf[first_slice_seg] & 0x80) != 0);
        }
    }

    return info;
}

static bool next_vcl_is_first_slice(AL_ECodec codec, std::vector<uint8_t> const &buf, size_t from)
{
    size_t scan = from;
    while (true)
    {
        StartCodeMatch sc = find_start_code(buf, scan);
        if (!sc.found)
        {
            return false;
        }

        StartCodeMatch next = find_start_code(buf, sc.offset + sc.size);
        if (!next.found)
        {
            return false;
        }

        NalInfo info = parse_nal_info(codec, buf, sc, next.offset);
        if (!info.valid)
        {
            scan = sc.offset + sc.size;
            continue;
        }

        if (info.is_vcl)
        {
            return info.is_first_slice;
        }

        scan = next.offset;
    }
}

SliceFeeder::SliceFeeder(AL_ECodec codec, uint32_t max_nal_size_bytes)
    : m_codec(codec), m_max_nal_sz(max_nal_size_bytes), m_read_chunk(max_nal_size_bytes)
{
    m_parse_buf.reserve(static_cast<size_t>(m_max_nal_sz) * 2);
}

bool SliceFeeder::set_error()
{
    m_failed = true;
    return false;
}

bool SliceFeeder::failed() const
{
    return m_failed;
}

bool SliceFeeder::emit_nal(size_t begin, size_t sc_size, size_t end, std::vector<uint8_t> &nal_out, uint8_t &flags_out)
{
    if (end <= begin || end > m_parse_buf.size())
    {
        VIDEO_ERROR_PRINT("Invalid NAL range [%zu, %zu) in slice feeder", begin, end);
        return set_error();
    }

    const size_t nal_size = end - begin;
    if (nal_size > static_cast<size_t>(m_max_nal_sz))
    {
        VIDEO_ERROR_PRINT("NAL unit (%zu bytes) exceeds input buffer size (%u)", nal_size, m_max_nal_sz);
        return set_error();
    }

    uint8_t flags = AL_STREAM_BUF_FLAG_ENDOFSLICE;
    NalInfo cur_info = parse_nal_info(m_codec, m_parse_buf, StartCodeMatch{begin, sc_size, true}, end);
    if (cur_info.valid && cur_info.is_vcl)
    {
        if (end < m_parse_buf.size())
        {
            if (next_vcl_is_first_slice(m_codec, m_parse_buf, end))
            {
                flags |= AL_STREAM_BUF_FLAG_ENDOFFRAME;
            }
        }
        else
        {
            flags |= AL_STREAM_BUF_FLAG_ENDOFFRAME;
        }
    }

    nal_out.assign(m_parse_buf.begin() + static_cast<std::vector<uint8_t>::difference_type>(begin),
                   m_parse_buf.begin() + static_cast<std::vector<uint8_t>::difference_type>(end));
    flags_out = flags;
    return true;
}

bool SliceFeeder::feed_frame(std::ifstream &input_file, std::vector<uint8_t> &frame_out)
{
    frame_out.clear();

    if (m_failed)
        return false;

    std::vector<uint8_t> nal;
    uint8_t flags = 0;

    while (feed(input_file, nal, flags))
    {
        frame_out.insert(frame_out.end(), nal.begin(), nal.end());
        if (flags & AL_STREAM_BUF_FLAG_ENDOFFRAME)
            return true;
    }

    // EOF with remaining non-VCL data (e.g. trailing SEI): treat as last frame.
    if (!frame_out.empty() && !m_failed)
        return true;

    return false;
}

bool SliceFeeder::feed(std::ifstream &input_file, std::vector<uint8_t> &nal_out, uint8_t &flags_out)
{
    nal_out.clear();
    flags_out = 0;

    if (m_failed)
    {
        return false;
    }

    while (true)
    {
        StartCodeMatch sc0 = find_start_code(m_parse_buf, 0);
        if (sc0.found)
        {
            StartCodeMatch sc1 = find_start_code(m_parse_buf, sc0.offset + sc0.size);
            if (sc1.found)
            {
                if (!emit_nal(sc0.offset, sc0.size, sc1.offset, nal_out, flags_out))
                {
                    return false;
                }

                m_parse_buf.erase(m_parse_buf.begin(),
                                  m_parse_buf.begin() + static_cast<std::vector<uint8_t>::difference_type>(sc1.offset));
                return true;
            }

            if (sc0.offset > 0)
            {
                m_parse_buf.erase(m_parse_buf.begin(),
                                  m_parse_buf.begin() + static_cast<std::vector<uint8_t>::difference_type>(sc0.offset));
                sc0.offset = 0;
            }

            if (m_eof)
            {
                if (m_parse_buf.empty())
                {
                    return false;
                }
                if (!emit_nal(0, sc0.size, m_parse_buf.size(), nal_out, flags_out))
                {
                    return false;
                }
                m_parse_buf.clear();
                return true;
            }
        }
        else if (m_eof)
        {
            m_parse_buf.clear();
            return false;
        }

        input_file.read(reinterpret_cast<char *>(m_read_chunk.data()),
                        static_cast<std::streamsize>(m_read_chunk.size()));
        std::streamsize bytes_read = input_file.gcount();
        if (bytes_read > 0)
        {
            m_parse_buf.insert(m_parse_buf.end(), m_read_chunk.begin(),
                               m_read_chunk.begin() + static_cast<std::vector<uint8_t>::difference_type>(bytes_read));
            continue;
        }

        if (input_file.bad())
        {
            VIDEO_ERROR_PRINT("Read error while feeding Annex-B stream");
            return set_error();
        }

        m_eof = true;
    }
}
