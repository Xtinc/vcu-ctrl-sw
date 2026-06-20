# lib_rtos — shared Linux/Windows OS abstraction layer
add_library(lib_rtos_obj OBJECT lib_rtos.c message.c)
target_link_libraries(lib_rtos_obj PUBLIC Threads::Threads)
