# lib_clock — clock management 
add_library(
    lib_network_obj
    OBJECT
    clock_wait.cpp
    udp_net.cpp
    udp_rs.c)

target_link_libraries(lib_network_obj PRIVATE lib_rtos_obj PUBLIC asio)
