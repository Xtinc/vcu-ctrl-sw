# lib_common — core data-structures and utility layer
# Compiled as an OBJECT library so the objects can be reused by multiple
# higher-level STATIC/SHARED libraries without repeated compilation.

set(LIB_COMMON_SRC
    Utils.c
    BufCommon.c
    AllocatorDefault.c
    AlignedAllocator.c
    MemDesc.c
    BufferAPI.c
    BufferPixMapMeta.c
    BufferHandleMeta.c
    Fifo.c
    FourCC.c
    HardwareDriver.c
    PixMapBuffer.c
    IntVector.c
    Planes.c
    HardwareConfig.c
    Error.c
    DisplayInfoMeta.c
    PicFormat.c
    StaticFifo.c
)

if(ENABLE_AVC)
    list(APPEND LIB_COMMON_SRC
        AvcLevelsLimit.c
        AvcUtils.c
    )
endif()

if(ENABLE_HEVC)
    list(APPEND LIB_COMMON_SRC
        HevcLevelsLimit.c
        HevcUtils.c
    )
endif()

if(ENABLE_CODEC)
    list(APPEND LIB_COMMON_SRC
        LevelLimit.c
        StreamBuffer.c
        ChannelResources.c
        HwScalingList.c
        SyntaxConversion.c
        BufferCircMeta.c
        BufferStreamMeta.c
        BufferPictureMeta.c
        BufferPictureDecMeta.c
        BufferSeiMeta.c
    )
    if(ENABLE_ENC_SW_MULTIPASS)
        list(APPEND LIB_COMMON_SRC BufferLookAheadMeta.c)
    endif()
    if(ENABLE_HIGH_DYNAMIC_RANGE)
        list(APPEND LIB_COMMON_SRC HDR.c)
    endif()
endif()

add_library(lib_common_obj OBJECT ${LIB_COMMON_SRC})
