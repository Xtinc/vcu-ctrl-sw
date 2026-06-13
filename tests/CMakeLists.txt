# Test / development-utility executables.
# These depend on app/ headers and core source files but are not part of
# the production firmware image.

set(APP_DIR ${CMAKE_SOURCE_DIR}/app)

# --- File-based encoder test ---
# Reads a raw YUV file and encodes it using the Allegro codec.
add_executable(vcu_enc_file
    ${APP_DIR}/MemMgr.cpp
    ${APP_DIR}/RTEncoder.cpp
    ${APP_DIR}/XilinxSyncIP.cpp
    YUVFileIO.cpp
    main_enc_file.cpp
)
target_include_directories(vcu_enc_file PRIVATE ${APP_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(vcu_enc_file PRIVATE liballegro_encode_static)

# --- File-based decoder test ---
# Opens a local HEVC/AVC bitstream file, decodes it, and displays via DRM.
# Uses RTDecoder + DRMDisplay directly (no DecMgr).
add_executable(vcu_dec_file
    ${APP_DIR}/MemMgr.cpp
    ${APP_DIR}/DRMDisplay.cpp
    ${APP_DIR}/RTDecoder.cpp
    SliceFeeder.cpp
    main_dec_file.cpp
)
target_include_directories(vcu_dec_file PRIVATE ${APP_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${LIBDRM_INCLUDE_DIRS})
target_link_libraries(vcu_dec_file PRIVATE liballegro_decode_static lib_network_obj ${LIBDRM_LIBRARIES})

# --- DRM dumb-buffer display test ---
# Exercises DRMDisplayDumb with synthetic solid-colour frames, no external
# DMA buffers or decoder required.
add_executable(dumb_test
    ${APP_DIR}/MemMgr.cpp
    ${APP_DIR}/DRMDisplay.cpp
    DRMDumbTest.cpp
)
target_include_directories(dumb_test PRIVATE ${APP_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${LIBDRM_INCLUDE_DIRS})
target_link_libraries(dumb_test PRIVATE liballegro_decode_static lib_network_obj ${LIBDRM_LIBRARIES})

# --- ReliableUDP protocol + FEC integration tests ---
# Covers RTT probes, deterministic XOR-FEC recovery cases, and random-sized
# messages through a configurable lossy UDP proxy.
#
# Usage:
#   ./test_reliable_udp [--min-bytes N] [--max-bytes N]
#                       [--rate-mbps F] [--loss-min F] [--loss-max F]
#                       [--duration N]
add_executable(test_reliable_udp
    test_reliable_udp.cpp)
target_link_libraries(test_reliable_udp PRIVATE lib_network_obj lib_rtos_obj Threads::Threads)

# --- RecvQueueAsync jitter/reorder unit tests ---
# Exercises in-order delivery, reorder recovery, duplicate suppression,
# adaptive depth, gap skip, output pacing smoothness, and SendQueue fill.
add_executable(test_reliable_udp_jitter
    test_reliable_udp_jitter.cpp)
target_link_libraries(test_reliable_udp_jitter PRIVATE lib_network_obj lib_rtos_obj Threads::Threads)

# --- RJEstimator unit test ---
# Feeds synthetic 2-100ms arrival intervals into the jitter estimator and
# validates interval tracking, jitter response, and outlier isolation.
add_executable(test_rj_estimator
    test_rj_estimator.cpp)
target_link_libraries(test_rj_estimator PRIVATE lib_network_obj lib_rtos_obj Threads::Threads)

# --- ReliableUDP TC jitter measurement utility ---
# Generates a local ReliableUDP stream and writes CSV metrics for offline
# analysis while external tc/netem rules inject jitter.
add_executable(test_reliable_udp_jitter_tc
    test_reliable_udp_jitter_tc.cpp)
target_link_libraries(test_reliable_udp_jitter_tc PRIVATE lib_network_obj lib_rtos_obj Threads::Threads)
