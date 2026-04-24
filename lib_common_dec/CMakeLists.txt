# lib_common_dec — decoder-side common data structures

set(LIB_COMMON_DEC_SRC
    BufferStatisticsMeta.c
    DecBuffers.c
    DecHardwareConfig.c
    DecHwScalingList.c
    DecInfo.c
    DecOutputSettings.c
    IpDecFourCC.c
    RbspParser.c
    StreamSettings.c
)

if(ENABLE_HIGH_DYNAMIC_RANGE)
    list(APPEND LIB_COMMON_DEC_SRC HDRMeta.c)
endif()

add_library(lib_common_dec_obj OBJECT ${LIB_COMMON_DEC_SRC})
