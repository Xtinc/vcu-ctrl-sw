# lib_encode — VCU hardware encoder
#
# Produces:
#   liballegro_encode        — shared library  (runtime)
#   liballegro_encode_static — static archive  (linked into AL_Encoder)

# ---- encode core sources (project_src.mk equivalent) ----
set(LIB_ENCODE_CORE_SRC
    AVC_Encoder.c
    AVC_EncUtils.c
    AVC_Sections.c
    Com_Encoder.c
    EncUtils.c
    HEVC_Encoder.c
    HEVC_EncUtils.c
    HEVC_Sections.c
    IP_Stream.c
    lib_encoder.c
    LibEncoderHost.c
    LoadLda.c
    NalWriters.c
    Sections.c
    SourceBufferChecker.c
)

add_library(lib_encode_core_obj OBJECT ${LIB_ENCODE_CORE_SRC})

# ---- IScheduler / MCU scheduler sources ----
set(LIB_ISCHEDULER_SRC
    DriverDataConversions.c
    EncSchedulerCommon.c
    EncSchedulerMcu.c
    I_EncScheduler.c
)

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
    $<TARGET_OBJECTS:lib_sync_obj>
)

add_library(liballegro_encode_static STATIC ${ALL_ENCODE_OBJECTS})

target_link_libraries(liballegro_encode_static PUBLIC Threads::Threads)
set_target_properties(liballegro_encode_static PROPERTIES OUTPUT_NAME allegro_encode)
set_target_properties(liballegro_encode_static PROPERTIES SUFFIX ".a")
