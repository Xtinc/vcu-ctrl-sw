cmake_minimum_required(VERSION 3.16)

project(vcu-ctrl-sw VERSION 0.94.0 LANGUAGES C CXX)

# Language standards: gnu11 / gnu++11 (matching -std=gnu11 / -std=gnu++11 in original)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# All objects compiled with -fPIC (required when linking into shared libraries)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Warning flags by compiler family
if(CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
    add_compile_options(/W4)
else()
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Werror=pedantic
        -Wwrite-strings
        -Wredundant-decls
        # -Wshadow
        -Wformat=2
        # -Wconversion
        # -Wnull-dereference
        -Wunreachable-code
    )
endif()

option(ENABLE_ENC_SW_MULTIPASS "Enable encoder software multi-pass" OFF)
add_compile_definitions(AL_ENABLE_64BIT=1)

find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBDRM REQUIRED libdrm)

# asio
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE asio-1.30.2/include)
target_compile_definitions(asio INTERFACE $<$<PLATFORM_ID:Windows>:_WIN32_WINNT=0x0601>)

# Global include paths
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

add_subdirectory(lib_bitstream)
add_subdirectory(lib_common_enc)

add_subdirectory(lib_common_dec)
add_subdirectory(lib_parsing)
add_subdirectory(lib_sync_ip)
add_subdirectory(lib_network)

# ---------- Batch 2: app-facing deliverables ----------
# User app layer + final SDK libraries consumed by applications.

add_subdirectory(lib_encode)
add_subdirectory(lib_decode)
add_subdirectory(app)
add_subdirectory(tests)