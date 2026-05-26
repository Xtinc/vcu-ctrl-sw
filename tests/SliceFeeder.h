#ifndef SLICE_INPUT_FEEDER_H
#define SLICE_INPUT_FEEDER_H

#include "lib_common/Profiles.h"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <vector>

class SliceFeeder
{
  public:
    explicit SliceFeeder(AL_ECodec codec, uint32_t max_nal_size_bytes);

    // Reads Annex-B bitstream and outputs one NAL per call.
    // Returns true when one NAL is produced; false on EOF or parse/read errors.
    bool feed(std::ifstream &input_file, std::vector<uint8_t> &nal_out, uint8_t &flags_out);

    // Reads Annex-B bitstream and accumulates NALs until a frame boundary
    // (AL_STREAM_BUF_FLAG_ENDOFFRAME) is reached, then returns the complete
    // concatenated frame data in frame_out.
    // Returns true when one frame is produced; false on EOF or parse/read errors.
    bool feed_frame(std::ifstream &input_file, std::vector<uint8_t> &frame_out);

    // Returns true if the previous feed() ended due to an error.
    bool failed() const;

  private:
    bool set_error();
    bool emit_nal(size_t begin, size_t sc_size, size_t end, std::vector<uint8_t> &nal_out, uint8_t &flags_out);

    AL_ECodec m_codec;
    uint32_t m_max_nal_sz;
    std::vector<uint8_t> m_read_chunk;
    std::vector<uint8_t> m_parse_buf;
    bool m_eof = false;
    bool m_failed = false;
};

#endif // SLICE_INPUT_FEEDER_H
