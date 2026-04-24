# lib_rtos — OS abstraction layer (Linux pthread backend)
add_library(lib_rtos_obj OBJECT lib_rtos.c)
target_link_libraries(lib_rtos_obj PUBLIC Threads::Threads)