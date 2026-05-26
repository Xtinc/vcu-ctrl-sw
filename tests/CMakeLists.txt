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
target_link_libraries(vcu_dec_file PRIVATE liballegro_decode_static ${LIBDRM_LIBRARIES})

# --- DRM dumb-buffer display test ---
# Exercises DRMDisplayDumb with synthetic solid-colour frames, no external
# DMA buffers or decoder required.
add_executable(dumb_test
    ${APP_DIR}/MemMgr.cpp
    ${APP_DIR}/DRMDisplay.cpp
    DRMDumbTest.cpp
    $<TARGET_OBJECTS:lib_network_obj>
)
target_include_directories(dumb_test PRIVATE ${APP_DIR} ${CMAKE_CURRENT_SOURCE_DIR} ${LIBDRM_INCLUDE_DIRS})
target_link_libraries(dumb_test PRIVATE liballegro_decode_static ${LIBDRM_LIBRARIES})
