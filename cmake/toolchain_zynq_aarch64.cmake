# Toolchain file for Zynq UltraScale+ MPSoC (ARM Cortex-A53, AArch64)
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain_zynq_aarch64.cmake \
#         -DCROSS_COMPILE=aarch64-linux-gnu- ..
#
# For Zynq-7000 (ARM Cortex-A9, 32-bit) change CMAKE_SYSTEM_PROCESSOR to "arm"
# and CROSS_COMPILE default to "arm-linux-gnueabihf-".

set(CMAKE_SYSTEM_NAME      Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CROSS_COMPILE "aarch64-linux-gnu-" CACHE STRING "Cross-compiler prefix")

set(CMAKE_C_COMPILER   ${CROSS_COMPILE}gcc)
set(CMAKE_CXX_COMPILER ${CROSS_COMPILE}g++)
set(CMAKE_AR           ${CROSS_COMPILE}gcc-ar)
set(CMAKE_RANLIB       ${CROSS_COMPILE}gcc-ranlib)
set(CMAKE_NM           ${CROSS_COMPILE}gcc-nm)
set(CMAKE_LINKER       ${CROSS_COMPILE}ld)
set(CMAKE_STRIP        ${CROSS_COMPILE}strip)
set(CMAKE_OBJCOPY      ${CROSS_COMPILE}objcopy)
set(CMAKE_OBJDUMP      ${CROSS_COMPILE}objdump)
set(CMAKE_SIZE         ${CROSS_COMPILE}size)

# Do not search host system for programs/libraries/headers
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
