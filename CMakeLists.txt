cmake_minimum_required(VERSION 3.16)

project(vcu-ctrl-sw
    VERSION 0.44.0
    LANGUAGES C CXX
)

# Language standards: gnu11 / gnu++11 (matching -std=gnu11 / -std=gnu++11 in original)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)

# All objects compiled with -fPIC (required when linking into shared libraries)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(cmake/options.cmake)
include(cmake/version.cmake)
include(cmake/compile_flags.cmake)

find_package(Threads REQUIRED)

# Global include paths (matches INCLUDES in codec_defs.mk)
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/extra/include
)

# ---------- Batch 1: SDK object components ----------
# Core SDK object libraries are grouped first.
add_subdirectory(lib_common)
add_subdirectory(lib_rtos)
add_subdirectory(lib_fpga)

if(ENABLE_ENCODER)
    add_subdirectory(lib_bitstream)
    add_subdirectory(lib_common_enc)
endif()

if(ENABLE_DECODER)
    add_subdirectory(lib_common_dec)
    add_subdirectory(lib_parsing)
endif()

# ---------- Batch 2: app-facing deliverables ----------
# User app layer + final SDK libraries consumed by applications.
add_subdirectory(lib_app)

if(ENABLE_ENCODER)
    add_subdirectory(lib_conv_yuv)
    add_subdirectory(lib_encode)
    add_subdirectory(exe_encoder)
endif()

if(ENABLE_DECODER)
    add_subdirectory(lib_decode)
    add_subdirectory(exe_decoder)
endif()

if(ENABLE_ENCODER)
    add_subdirectory(realtime_encoder)
endif()