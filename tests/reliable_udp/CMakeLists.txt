function(add_udp_test target source)
    add_executable(${target} ${source})
    target_link_libraries(${target} PRIVATE lib_network)
endfunction()

add_udp_test(test_udp_protocol protocol_test.cpp)
add_udp_test(test_udp_queue queue_test.cpp)
add_udp_test(test_udp_estimator estimator_test.cpp)
add_udp_test(test_udp_layout layout_test.cpp)
add_udp_test(test_udp_network network_test.cpp)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_udp_test(test_udp_tc tc_test.cpp)
endif()
