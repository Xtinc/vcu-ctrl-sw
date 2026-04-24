# lib_bitstream — bitstream writing helpers (AVC/HEVC RBSP, CABAC)

set(LIB_BITSTREAM_SRC
    BitStreamLite.c
    RbspEncod.c
)

if(ENABLE_ENC_ITU)
    list(APPEND LIB_BITSTREAM_SRC Cabac.c)
endif()

if(ENABLE_ENC_AVC)
    list(APPEND LIB_BITSTREAM_SRC
        AVC_RbspEncod.c
        AVC_SkippedPict.c
    )
endif()

if(ENABLE_ENC_HEVC)
    list(APPEND LIB_BITSTREAM_SRC
        HEVC_RbspEncod.c
        HEVC_SkippedPict.c
    )
endif()

add_library(lib_bitstream_obj OBJECT ${LIB_BITSTREAM_SRC})
