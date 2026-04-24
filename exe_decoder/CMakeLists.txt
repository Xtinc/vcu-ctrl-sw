# exe_decoder — AL_Decoder application

# ---- version / introspection defines for CodecUtils.cpp ----
set(DECODER_CODECUTILS_DEFS
    "SCM_REV_SW=\"${SCM_REV_SW}\""
    "SCM_BRANCH=\"${SCM_BRANCH}\""
    "DELIVERY_BUILD_NUMBER=${DELIVERY_BUILD_NUMBER}"
    "DELIVERY_SCM_REV=\"${DELIVERY_SCM_REV}\""
    "DELIVERY_DATE=\"${DELIVERY_DATE}\""
    "AL_COMPIL_FLAGS=\"\""
    HAS_COMPIL_FLAGS=1
)

set(EXE_DECODER_SRC
    main.cpp
    SinkYuvCrc.cpp
    CmdParser.cpp
    IpDevice.cpp
    IpDeviceCommon.cpp
    CodecUtils.cpp
    InputLoader.cpp
    SinkCheckSum.cpp
    SinkStatistics.cpp
    SinkYuvMd5.cpp
)

if(ENABLE_HIGH_DYNAMIC_RANGE)
    list(APPEND EXE_DECODER_SRC HDRWriter.cpp)
endif()

add_executable(AL_Decoder ${EXE_DECODER_SRC})

target_compile_definitions(AL_Decoder PRIVATE ${DECODER_CODECUTILS_DEFS})

target_link_libraries(AL_Decoder PRIVATE
    liballegro_decode_static
    liballegro_app_static
    liballegro_conv_yuv_static
    Threads::Threads
)

set_target_properties(AL_Decoder PROPERTIES OUTPUT_NAME AL_Decoder)
