# lib_fpga — hardware DMA allocator and device pool
# On Linux with ENABLE_LINUX_DRIVER: real DMA allocator via kernel driver.
# Fallback: stub (BoardNone.c) for simulation / host builds.

if(ENABLE_LINUX_DRIVER AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LIB_FPGA_SRC
        DmaAllocLinux.c
        DevicePool.c
    )
else()
    set(LIB_FPGA_SRC BoardNone.c)
endif()

add_library(lib_fpga_obj OBJECT ${LIB_FPGA_SRC})
