# ReliableUDP transport, queues, timing, and FEC
add_library(
    lib_network_obj
    OBJECT
    ClockWait.cpp
    CSVWriter.cpp
    ReliableUDP.cpp
    ReedSoloman.c
    QueueAsync.cpp)

target_link_libraries(lib_network_obj PUBLIC asio)

# Public aggregate target: consumers get both network and RTOS object files
# without repeating the implementation targets at every link site.
add_library(lib_network INTERFACE)
target_sources(lib_network INTERFACE
    $<TARGET_OBJECTS:lib_network_obj>
    $<TARGET_OBJECTS:lib_rtos_obj>)
target_link_libraries(lib_network INTERFACE asio Threads::Threads)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    target_link_libraries(lib_network INTERFACE ws2_32 mswsock)
endif()
