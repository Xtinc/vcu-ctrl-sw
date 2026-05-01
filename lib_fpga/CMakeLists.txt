# lib_fpga — hardware DMA allocator and device pool
# Fallback: stub (BoardNone.c) for simulation / host builds.

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    set(LIB_FPGA_SRC
        DmaAllocLinux.c
        DevicePool.c
    )
else()
    set(LIB_FPGA_SRC BoardNone.c)
endif()

add_library(lib_fpga_obj OBJECT ${LIB_FPGA_SRC})
