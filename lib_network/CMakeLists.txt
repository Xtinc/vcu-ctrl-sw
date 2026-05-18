# lib_clock — clock management 
add_library(lib_network_obj OBJECT clock_wait.cpp)
target_link_libraries(lib_network_obj PRIVATE lib_rtos_obj)
target_link_libraries(lib_network_obj PRIVATE asio)