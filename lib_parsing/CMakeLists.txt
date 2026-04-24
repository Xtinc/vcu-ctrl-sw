# lib_parsing — NAL/slice header parsing (AVC + HEVC)

set(LIB_PARSING_SRC
    SeiParser.c
    Avc_PictMngr.c
    Avc_SliceHeaderParsing.c
    AvcParser.c
    common_syntax.c
    Concealment.c
    DPB.c
    Hevc_PictMngr.c
    Hevc_SliceHeaderParsing.c
    HevcParser.c
    I_PictMngr.c
)

add_library(lib_parsing_obj OBJECT ${LIB_PARSING_SRC})
