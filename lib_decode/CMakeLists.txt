# lib_decode — VCU hardware decoder
#
# Produces:
#   liballegro_decode        — shared library  (runtime)
#   liballegro_decode_static — static archive  (linked into AL_Decoder)

# ---- decode core sources (project_src.mk equivalent) ----
set(LIB_DECODE_CORE_SRC
    NalUnitParser.c
    FrameParam.c
    DefaultDecoder.c
    LibDecoderHost.c
    lib_decode.c
    UnsplitBufferFeeder.c
    Patchworker.c
    DecoderFeeder.c
    SplitBufferFeeder.c
    I_DecScheduler.c
    WorkPool.c
    DecSettings.c
)

if(ENABLE_DEC_ITU)
    list(APPEND LIB_DECODE_CORE_SRC SliceDataParsing.c)
endif()

if(ENABLE_MICROBLAZE)
    list(APPEND LIB_DECODE_CORE_SRC DecSchedulerMcu.c)
endif()

if(ENABLE_DEC_AVC)
    list(APPEND LIB_DECODE_CORE_SRC
        AvcDecoder.c
        AvcHwBufInitialization.c
    )
endif()

if(ENABLE_DEC_HEVC)
    list(APPEND LIB_DECODE_CORE_SRC
        HevcDecoder.c
        HevcHwBufInitialization.c
    )
endif()

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

add_library(liballegro_decode        SHARED ${ALL_DECODE_OBJECTS})
add_library(liballegro_decode_static STATIC ${ALL_DECODE_OBJECTS})

foreach(_tgt liballegro_decode liballegro_decode_static)
    target_link_libraries(${_tgt} PUBLIC Threads::Threads)
    set_target_properties(${_tgt} PROPERTIES OUTPUT_NAME allegro_decode)
endforeach()

set_target_properties(liballegro_decode_static PROPERTIES SUFFIX ".a")
