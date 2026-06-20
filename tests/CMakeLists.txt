# Test / development-utility executables.
# Linux hardware utilities and cross-platform network tests.

set(APP_DIR ${CMAKE_SOURCE_DIR}/app)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
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
    # Opens a local HEVC/AVC bitstream, decodes it, and displays via DRM.
    add_executable(vcu_dec_file
        ${APP_DIR}/MemMgr.cpp
        ${APP_DIR}/DRMDisplay.cpp
        ${APP_DIR}/RTDecoder.cpp
        SliceFeeder.cpp
        main_dec_file.cpp
    )
    target_include_directories(vcu_dec_file PRIVATE ${APP_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${LIBDRM_INCLUDE_DIRS})
    target_link_libraries(vcu_dec_file PRIVATE liballegro_decode_static lib_network ${LIBDRM_LIBRARIES})

    # --- DRM dumb-buffer display test ---
    add_executable(dumb_test
        ${APP_DIR}/MemMgr.cpp
        ${APP_DIR}/DRMDisplay.cpp
        DRMDumbTest.cpp
    )
    target_include_directories(dumb_test PRIVATE ${APP_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${LIBDRM_INCLUDE_DIRS})
    target_link_libraries(dumb_test PRIVATE liballegro_decode_static lib_network ${LIBDRM_LIBRARIES})
endif()

add_subdirectory(reliable_udp)
