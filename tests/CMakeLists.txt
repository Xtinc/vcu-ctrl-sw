add_executable(test_clock_wait test_clock_wait.cpp)

target_include_directories(test_clock_wait PRIVATE ${CMAKE_SOURCE_DIR})

target_link_libraries(test_clock_wait
    PRIVATE
        lib_network_obj
        lib_rtos_obj
        Threads::Threads
)
