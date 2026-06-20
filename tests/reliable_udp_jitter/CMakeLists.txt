# Cross-platform ReliableUDP jitter/controller tests.
add_executable(test_reliable_udp_jitter test_reliable_udp_jitter.cpp)
target_link_libraries(test_reliable_udp_jitter PRIVATE lib_network)

add_executable(test_rj_estimator test_rj_estimator.cpp)
target_link_libraries(test_rj_estimator PRIVATE lib_network)

add_executable(test_reliable_udp_layout test_reliable_udp_layout.cpp)
target_link_libraries(test_reliable_udp_layout PRIVATE lib_network)

add_executable(test_reliable_udp_network test_reliable_udp_network.cpp)
target_link_libraries(test_reliable_udp_network PRIVATE lib_network)

# tc/netem and its shell runner are intentionally Linux-only.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(test_reliable_udp_jitter_tc test_reliable_udp_jitter_tc.cpp)
    target_link_libraries(test_reliable_udp_jitter_tc PRIVATE lib_network)
endif()
