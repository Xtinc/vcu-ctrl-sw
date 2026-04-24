# lib_bitstream — bitstream writing helpers (AVC/HEVC RBSP, CABAC)

set(LIB_BITSTREAM_SRC
    AVC_RbspEncod.c
    AVC_SkippedPict.c
    BitStreamLite.c
    Cabac.c
    HEVC_RbspEncod.c
    HEVC_SkippedPict.c
    RbspEncod.c
)

add_library(lib_bitstream_obj OBJECT ${LIB_BITSTREAM_SRC})
