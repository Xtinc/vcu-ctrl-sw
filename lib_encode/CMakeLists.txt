# lib_encode — VCU hardware encoder
#
# Produces:
#   liballegro_encode        — shared library  (runtime)
#   liballegro_encode_static — static archive  (linked into AL_Encoder)

# ---- encode core sources (project_src.mk equivalent) ----
set(LIB_ENCODE_CORE_SRC
    Com_Encoder.c
    Sections.c
    NalWriters.c
    EncUtils.c
    IP_Stream.c
    LibEncoderHost.c
    lib_encoder.c
    SourceBufferChecker.c
    LoadLda.c
)

if(ENABLE_ENC_AVC)
    list(APPEND LIB_ENCODE_CORE_SRC
        AVC_EncUtils.c
        AVC_Encoder.c
        AVC_Sections.c
    )
endif()

if(ENABLE_ENC_HEVC)
    list(APPEND LIB_ENCODE_CORE_SRC
        HEVC_EncUtils.c
        HEVC_Encoder.c
        HEVC_Sections.c
    )
endif()

add_library(lib_encode_core_obj OBJECT ${LIB_ENCODE_CORE_SRC})

# ---- IScheduler / MCU scheduler sources ----
set(LIB_ISCHEDULER_SRC
    I_EncScheduler.c
    EncSchedulerCommon.c
)

if(ENABLE_MICROBLAZE)
    list(APPEND LIB_ISCHEDULER_SRC
        DriverDataConversions.c
        EncSchedulerMcu.c
    )
endif()

add_library(lib_ischeduler_enc_obj OBJECT ${LIB_ISCHEDULER_SRC})

# ---- monolithic liballegro_encode ----
set(ALL_ENCODE_OBJECTS
    $<TARGET_OBJECTS:lib_encode_core_obj>
    $<TARGET_OBJECTS:lib_ischeduler_enc_obj>
    $<TARGET_OBJECTS:lib_bitstream_obj>
    $<TARGET_OBJECTS:lib_fpga_obj>
    $<TARGET_OBJECTS:lib_common_obj>
    $<TARGET_OBJECTS:lib_common_enc_obj>
    $<TARGET_OBJECTS:lib_rtos_obj>
)

add_library(liballegro_encode        SHARED ${ALL_ENCODE_OBJECTS})
add_library(liballegro_encode_static STATIC ${ALL_ENCODE_OBJECTS})

foreach(_tgt liballegro_encode liballegro_encode_static)
    target_link_libraries(${_tgt} PUBLIC Threads::Threads)
    set_target_properties(${_tgt} PROPERTIES OUTPUT_NAME allegro_encode)
endforeach()

set_target_properties(liballegro_encode_static PROPERTIES SUFFIX ".a")
