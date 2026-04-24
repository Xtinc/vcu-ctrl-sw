# exe_encoder — AL_Encoder and AL_CfgParser applications

# ---- version / introspection defines for main.cpp ----
set(ENCODER_MAIN_DEFS
    "SCM_REV_SW=\"${SCM_REV_SW}\""
    "SCM_BRANCH=\"${SCM_BRANCH}\""
    "DELIVERY_BUILD_NUMBER=${DELIVERY_BUILD_NUMBER}"
    "DELIVERY_SCM_REV=\"${DELIVERY_SCM_REV}\""
    "DELIVERY_DATE=\"${DELIVERY_DATE}\""
    "AL_COMPIL_FLAGS=\"\""
    HAS_COMPIL_FLAGS=1
)

# ---- AL_Encoder ----
set(EXE_ENCODER_SRC
    CodecUtils.cpp
    IpDevice.cpp
    container.cpp
    main.cpp
    sink_bitstream_writer.cpp
    sink_bitrate.cpp
    sink_yuv_md5.cpp
    EncCmdMngr.cpp
    QPGenerator.cpp
    CommandsSender.cpp
    CfgParser.cpp
)

if(ENABLE_ROI)
    list(APPEND EXE_ENCODER_SRC ROIMngr.cpp)
endif()

if(ENABLE_ENC_SW_MULTIPASS)
    list(APPEND EXE_ENCODER_SRC TwoPassMngr.cpp)
endif()

if(ENABLE_HIGH_DYNAMIC_RANGE)
    list(APPEND EXE_ENCODER_SRC HDRParser.cpp)
endif()

add_executable(AL_Encoder ${EXE_ENCODER_SRC})

target_compile_definitions(AL_Encoder PRIVATE ${ENCODER_MAIN_DEFS})

target_link_libraries(AL_Encoder PRIVATE
    liballegro_encode_static
    liballegro_app_static
    liballegro_conv_yuv_static
    Threads::Threads
)

set_target_properties(AL_Encoder PROPERTIES OUTPUT_NAME AL_Encoder)

# ---- AL_CfgParser ----
add_executable(AL_CfgParser
    ParserMain.cpp
    CfgParser.cpp
)

target_link_libraries(AL_CfgParser PRIVATE
    liballegro_encode_static
    liballegro_app_static
    Threads::Threads
)

set_target_properties(AL_CfgParser PROPERTIES OUTPUT_NAME AL_CfgParser)
