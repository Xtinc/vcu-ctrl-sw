# lib_common — core data-structures and utility layer
# Compiled as an OBJECT library so the objects can be reused by multiple
# higher-level STATIC/SHARED libraries without repeated compilation.

set(LIB_COMMON_SRC
    AlignedAllocator.c
    AllocatorDefault.c
    AvcLevelsLimit.c
    AvcUtils.c
    BufCommon.c
    BufferAPI.c
    BufferCircMeta.c
    BufferHandleMeta.c
    BufferPictureDecMeta.c
    BufferPictureMeta.c
    BufferPixMapMeta.c
    BufferSeiMeta.c
    BufferStreamMeta.c
    ChannelResources.c
    DisplayInfoMeta.c
    Error.c
    Fifo.c
    FourCC.c
    HardwareConfig.c
    HardwareDriver.c
    HevcLevelsLimit.c
    HevcUtils.c
    HwScalingList.c
    IntVector.c
    LevelLimit.c
    MemDesc.c
    PicFormat.c
    PixMapBuffer.c
    Planes.c
    StreamBuffer.c
    SyntaxConversion.c
    Utils.c
)


if(ENABLE_ENC_SW_MULTIPASS)
    list(APPEND LIB_COMMON_SRC BufferLookAheadMeta.c)
endif()
if(ENABLE_HIGH_DYNAMIC_RANGE)
    list(APPEND LIB_COMMON_SRC HDR.c)
endif()

add_library(lib_common_obj OBJECT ${LIB_COMMON_SRC})
