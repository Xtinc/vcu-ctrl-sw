# Sources shared by both apps
set(APP_COMMON_SRC
    ClockSync.cpp
    LatencyStats.cpp
    MemMgr.cpp
    YUVFileIO.cpp
)

# --- Encoder app ---
add_executable(vcu_enc
    main_enc.cpp
    ${APP_COMMON_SRC}
    RTEncoder.cpp
    RTEncoder.h
    V4L2Source.cpp
    V4L2Source.h
    XilinxSyncIP.cpp
    XilinxSyncIP.h
    DMAFd.h
)
target_link_libraries(vcu_enc
    PRIVATE
    liballegro_encode_static
    liballegro_decode_static
    asio
)

# --- Decoder-to-YUV app ---
add_executable(vcu_dec
    main_dec.cpp
    ${APP_COMMON_SRC}
    RTDecoder.cpp
    RTDecoder.h
)
target_link_libraries(vcu_dec
    PRIVATE
    liballegro_encode_static
    liballegro_decode_static
    asio
)
