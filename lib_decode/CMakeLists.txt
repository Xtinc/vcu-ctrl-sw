# lib_decode — VCU hardware decoder
#
# Produces:
#   liballegro_decode        — shared library  (runtime)
#   liballegro_decode_static — static archive  (linked into AL_Decoder)

# ---- decode core sources (project_src.mk equivalent) ----
set(LIB_DECODE_CORE_SRC
    AvcDecoder.c
    AvcHwBufInitialization.c
    DecoderFeeder.c
    DecSchedulerMcu.c
    DecSettings.c
    DefaultDecoder.c
    FrameParam.c
    HevcDecoder.c
    HevcHwBufInitialization.c
    I_DecScheduler.c
    lib_decode.c
    LibDecoderHost.c
    NalUnitParser.c
    Patchworker.c
    SliceDataParsing.c
    SplitBufferFeeder.c
    UnsplitBufferFeeder.c
    WorkPool.c
)

add_library(lib_decode_core_obj OBJECT ${LIB_DECODE_CORE_SRC})

# ---- monolithic liballegro_decode ----
set(ALL_DECODE_OBJECTS
    $<TARGET_OBJECTS:lib_decode_core_obj>
    $<TARGET_OBJECTS:lib_rtos_obj>
    $<TARGET_OBJECTS:lib_common_obj>
    $<TARGET_OBJECTS:lib_common_dec_obj>
    $<TARGET_OBJECTS:lib_fpga_obj>
    $<TARGET_OBJECTS:lib_parsing_obj>
)

add_library(liballegro_decode_static STATIC ${ALL_DECODE_OBJECTS})
target_link_libraries(liballegro_decode_static PUBLIC Threads::Threads)
set_target_properties(liballegro_decode_static PROPERTIES OUTPUT_NAME allegro_decode)
set_target_properties(liballegro_decode_static PROPERTIES SUFFIX ".a")
