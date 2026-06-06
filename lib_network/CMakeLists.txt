# lib_clock — clock management 
add_library(
    lib_network_obj
    OBJECT
    ClockWait.cpp
    ReliableUDP.cpp
    ReedSoloman.c
    QueueAsync.cpp)

target_link_libraries(lib_network_obj PRIVATE lib_rtos_obj PUBLIC asio)
