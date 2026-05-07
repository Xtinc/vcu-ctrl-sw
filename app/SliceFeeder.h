#ifndef SLICE_INPUT_FEEDER_H
#define SLICE_INPUT_FEEDER_H

#include "RTDecoder.h"
#include <fstream>

class SliceFeeder
{
  public:
    explicit SliceFeeder(AL_ECodec codec, uint32_t max_nal_size_bytes);

    // Reads Annex-B bitstream and pushes one NAL per call into decoder.
    // Returns true on success, false on parsing/push errors.
    bool feed(std::ifstream &input_file, RTDecoder &decoder);

  private:
    AL_ECodec m_codec;
    uint32_t m_max_nal_sz;
    std::vector<uint8_t> m_read_chunk;
    std::vector<uint8_t> m_parse_buf;
};

#endif // SLICE_INPUT_FEEDER_H
