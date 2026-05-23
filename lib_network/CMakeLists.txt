# lib_clock — clock management 
add_library(lib_network_obj OBJECT clock_wait.cpp)
target_link_libraries(lib_network_obj PRIVATE lib_rtos_obj)
target_link_libraries(lib_network_obj PRIVATE asio)

# clock_wait accuracy / correctness test
add_executable(clock_wait_test clock_wait_test.cpp clock_wait.cpp)
target_link_libraries(clock_wait_test PRIVATE lib_rtos_obj asio Threads::Threads)
target_include_directories(clock_wait_test PRIVATE ${CMAKE_SOURCE_DIR} ${CMAKE_SOURCE_DIR}/include ${CMAKE_SOURCE_DIR}/extra/include)

include(CTest)
add_test(NAME clock_wait_accuracy COMMAND clock_wait_test)