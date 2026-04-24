# lib_conv_yuv — YUV raster-format conversion
#
# Produces:
#   liballegro_conv_yuv        — shared library
#   liballegro_conv_yuv_static — static archive

set(ALL_CONV_YUV_OBJECTS
    AL_RasterConvert.cpp
    $<TARGET_OBJECTS:lib_common_obj>
    $<TARGET_OBJECTS:lib_common_enc_obj>
    $<TARGET_OBJECTS:lib_rtos_obj>
)

add_library(liballegro_conv_yuv        SHARED ${ALL_CONV_YUV_OBJECTS})
add_library(liballegro_conv_yuv_static STATIC ${ALL_CONV_YUV_OBJECTS})

foreach(_tgt liballegro_conv_yuv liballegro_conv_yuv_static)
    set_target_properties(${_tgt} PROPERTIES OUTPUT_NAME allegro_conv_yuv)
endforeach()

target_link_libraries(liballegro_conv_yuv PRIVATE
    liballegro_app_static
    Threads::Threads
)

target_link_libraries(liballegro_conv_yuv_static INTERFACE
    liballegro_app_static
    Threads::Threads
)

set_target_properties(liballegro_conv_yuv_static PROPERTIES SUFFIX ".a")
