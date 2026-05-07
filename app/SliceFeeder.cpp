#include "SliceFeeder.h"

extern "C"
{
#include "lib_common/AvcUtils.h"
#include "lib_common/HevcUtils.h"
#include "lib_common/Nuts.h"
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

SliceFeeder::SliceFeeder(AL_ECodec codec, uint32_t max_nal_size_bytes)
    : m_codec(codec), m_max_nal_sz(max_nal_size_bytes), m_read_chunk(max_nal_size_bytes)
{
    m_parse_buf.reserve(static_cast<size_t>(m_max_nal_sz) * 2);
}

bool SliceFeeder::feed(std::ifstream &input_file, RTDecoder &decoder)
{
    m_parse_buf.clear();

    while (input_file.good())
    {
        input_file.read(reinterpret_cast<char *>(m_read_chunk.data()),
                        static_cast<std::streamsize>(m_read_chunk.size()));
        std::streamsize bytes_read = input_file.gcount();
        if (bytes_read > 0)
        {
            m_parse_buf.insert(m_parse_buf.end(), m_read_chunk.begin(), m_read_chunk.begin() + bytes_read);
        }

        while (true)
        {
            StartCodeMatch sc0 = find_start_code(m_parse_buf, 0);
            if (!sc0.found)
            {
                break;
            }

            StartCodeMatch sc1 = find_start_code(m_parse_buf, sc0.offset + sc0.size);
            if (!sc1.found)
            {
                if (sc0.offset > 0)
                {
                    m_parse_buf.erase(m_parse_buf.begin(),
                                      m_parse_buf.begin() +
                                          static_cast<std::vector<uint8_t>::difference_type>(sc0.offset));
                }
                break;
            }

            const size_t nal_size = sc1.offset - sc0.offset;
            if (nal_size > static_cast<size_t>(m_max_nal_sz))
            {
                VIDEO_ERROR_PRINT("NAL unit (%zu bytes) exceeds input buffer size (%u)", nal_size, m_max_nal_sz);
                return false;
            }

            uint8_t flags = AL_STREAM_BUF_FLAG_ENDOFSLICE;

            // Mark frame boundary when current VCL is followed by a first-slice VCL.
            const size_t cur_hdr = sc0.offset + sc0.size;
            const size_t nxt_hdr = sc1.offset + sc1.size;
            if (m_codec == AL_CODEC_AVC && (cur_hdr < m_parse_buf.size()) && (nxt_hdr + 1 < m_parse_buf.size()))
            {
                AL_ENut cur_nut = static_cast<AL_ENut>(m_parse_buf[cur_hdr] & 0x1F);
                AL_ENut nxt_nut = static_cast<AL_ENut>(m_parse_buf[nxt_hdr] & 0x1F);
                if (AL_AVC_IsVcl(cur_nut) && AL_AVC_IsVcl(nxt_nut) && (m_parse_buf[nxt_hdr + 1] & 0x80))
                {
                    flags |= AL_STREAM_BUF_FLAG_ENDOFFRAME;
                }
            }
            else if (m_codec == AL_CODEC_HEVC && (cur_hdr + 1 < m_parse_buf.size()) &&
                     (nxt_hdr + 2 < m_parse_buf.size()))
            {
                AL_ENut cur_nut = static_cast<AL_ENut>((m_parse_buf[cur_hdr] >> 1) & 0x3F);
                AL_ENut nxt_nut = static_cast<AL_ENut>((m_parse_buf[nxt_hdr] >> 1) & 0x3F);
                if (AL_HEVC_IsVcl(cur_nut) && AL_HEVC_IsVcl(nxt_nut) && (m_parse_buf[nxt_hdr + 2] & 0x80))
                {
                    flags |= AL_STREAM_BUF_FLAG_ENDOFFRAME;
                }
            }

            if (!decoder.push_stream(m_parse_buf.data() + sc0.offset, nal_size, flags))
            {
                VIDEO_ERROR_PRINT("push_stream failed in slice mode - decoder has stopped");
                return false;
            }

            m_parse_buf.erase(m_parse_buf.begin(),
                              m_parse_buf.begin() + static_cast<std::vector<uint8_t>::difference_type>(sc1.offset));
        }

        if (bytes_read <= 0)
        {
            break;
        }
    }

    // Push trailing NAL at EOF.
    StartCodeMatch sc = find_start_code(m_parse_buf, 0);
    if (sc.found && (sc.offset < m_parse_buf.size()))
    {
        if (sc.offset > 0)
        {
            m_parse_buf.erase(m_parse_buf.begin(),
                              m_parse_buf.begin() + static_cast<std::vector<uint8_t>::difference_type>(sc.offset));
        }

        if (!m_parse_buf.empty())
        {
            if (m_parse_buf.size() > static_cast<size_t>(m_max_nal_sz))
            {
                VIDEO_ERROR_PRINT("Trailing NAL (%zu bytes) exceeds input buffer size (%u)", m_parse_buf.size(),
                                  m_max_nal_sz);
                return false;
            }

            if (!decoder.push_stream(m_parse_buf.data(), m_parse_buf.size(), AL_STREAM_BUF_FLAG_ENDOFSLICE))
            {
                VIDEO_ERROR_PRINT("push_stream failed for trailing NAL in slice mode");
                return false;
            }
        }
    }

    return true;
}
