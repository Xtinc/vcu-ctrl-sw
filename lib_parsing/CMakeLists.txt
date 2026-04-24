# lib_parsing — NAL/slice header parsing (AVC + HEVC)

set(LIB_PARSING_SRC
    common_syntax.c
    DPB.c
    I_PictMngr.c
    Concealment.c
)

if(ENABLE_DEC_ITU)
    list(APPEND LIB_PARSING_SRC SeiParser.c)
endif()

if(ENABLE_DEC_AVC)
    list(APPEND LIB_PARSING_SRC
        AvcParser.c
        Avc_PictMngr.c
        Avc_SliceHeaderParsing.c
    )
endif()

if(ENABLE_DEC_HEVC)
    list(APPEND LIB_PARSING_SRC
        HevcParser.c
        Hevc_PictMngr.c
        Hevc_SliceHeaderParsing.c
    )
endif()

add_library(lib_parsing_obj OBJECT ${LIB_PARSING_SRC})
