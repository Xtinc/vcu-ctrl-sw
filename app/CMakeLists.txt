# Source layout: only three groups (common / encoder / decoder)
set(APP_COMMON_SRC
    MemMgr.cpp
)

set(APP_ENC_CORE_SRC
    RTEncoder.cpp
    XilinxSyncIP.cpp
)

set(APP_ENC_FILE_SRC
    YUVFileIO.cpp
    main_enc_file.cpp
)

set(APP_ENC_V4L2_SRC
    EncMgr.cpp
    V4L2Source.cpp
    main_enc_v4l2.cpp
)

set(APP_DEC_SRC
    DRMDisplay.cpp
    RTDecoder.cpp
    SliceFeeder.cpp
    main_dec.cpp
)

# --- Encoder file app ---
add_executable(vcu_enc_file
    ${APP_COMMON_SRC}
    ${APP_ENC_CORE_SRC}
    ${APP_ENC_FILE_SRC}
)
target_link_libraries(vcu_enc_file PRIVATE liballegro_encode_static)

# --- Encoder v4l2 app ---
add_executable(vcu_enc_v4l2
    ${APP_COMMON_SRC}
    ${APP_ENC_CORE_SRC}
    ${APP_ENC_V4L2_SRC}
)
target_link_libraries(vcu_enc_v4l2 PRIVATE liballegro_encode_static)

# --- Decoder app ---
add_executable(vcu_dec
    ${APP_COMMON_SRC}
    ${APP_DEC_SRC}
)
target_link_libraries(vcu_dec PRIVATE liballegro_decode_static ${LIBDRM_LIBRARIES})
target_include_directories(vcu_dec PRIVATE ${LIBDRM_INCLUDE_DIRS})

