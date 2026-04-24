# SPDX-FileCopyrightText: © 2024 Allegro DVT <github-ip@allegrodvt.com>
# SPDX-License-Identifier: MIT

cmake_minimum_required(VERSION 3.10)

option(ENABLE_V4L2_SOURCE "Enable V4L2 dmabuf source (Linux only)" ON)

if(ENABLE_V4L2_SOURCE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(STATUS "V4L2 source disabled: only supported on Linux")
    set(ENABLE_V4L2_SOURCE OFF)
endif()

# ---- 库目标 ----
set(REALTIME_ENCODER_SRCS
    RealtimeEncoder.cpp
    YuvFileSource.cpp
)

if(ENABLE_V4L2_SOURCE)
    list(APPEND REALTIME_ENCODER_SRCS
        V4l2DmabufSource.cpp
    )
endif()

add_library(realtime_encoder STATIC ${REALTIME_ENCODER_SRCS})

target_include_directories(realtime_encoder
    PUBLIC
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/..
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_features(realtime_encoder PUBLIC cxx_std_14)
target_compile_definitions(realtime_encoder PUBLIC
    $<$<BOOL:${ENABLE_V4L2_SOURCE}>:REALTIME_ENCODER_HAS_V4L2=1>
    $<$<NOT:$<BOOL:${ENABLE_V4L2_SOURCE}>>:REALTIME_ENCODER_HAS_V4L2=0>
)
if(MSVC)
    target_compile_options(realtime_encoder PRIVATE /W4 /wd4100)
else()
    target_compile_options(realtime_encoder PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

target_link_libraries(realtime_encoder
    PUBLIC
        allegro_encode
        liballegro_app_static
        liballegro_conv_yuv_static
        liballegro_common_static
        liballegro_common_enc_static
        liballegro_bitstream_static
        liballegro_rtos_static
        liballegro_fpga_static
)

# ---- 可选：示例可执行文件 ----
option(BUILD_REALTIME_ENCODER_EXAMPLE "Build realtime encoder example" OFF)

if(BUILD_REALTIME_ENCODER_EXAMPLE)
    add_executable(realtime_encoder_example
        example/main.cpp
    )
    target_include_directories(realtime_encoder_example PRIVATE
        ${CMAKE_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../include
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    target_link_libraries(realtime_encoder_example PRIVATE
        realtime_encoder
    )
    target_compile_features(realtime_encoder_example PRIVATE cxx_std_14)
    set_target_properties(realtime_encoder_example PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()
