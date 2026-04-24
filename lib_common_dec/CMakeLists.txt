# lib_common_dec — decoder-side common data structures

set(LIB_COMMON_DEC_SRC
    DecBuffers.c
    DecInfo.c
    DecHardwareConfig.c
    RbspParser.c
    IpDecFourCC.c
    StreamSettings.c
    DecOutputSettings.c
)

if(ENABLE_DECODER)
    list(APPEND LIB_COMMON_DEC_SRC DecHwScalingList.c)
    if(ENABLE_HIGH_DYNAMIC_RANGE)
        list(APPEND LIB_COMMON_DEC_SRC HDRMeta.c)
    endif()
endif()

if(ENABLE_DEC_CODEC)
    list(APPEND LIB_COMMON_DEC_SRC BufferStatisticsMeta.c)
endif()

add_library(lib_common_dec_obj OBJECT ${LIB_COMMON_DEC_SRC})
