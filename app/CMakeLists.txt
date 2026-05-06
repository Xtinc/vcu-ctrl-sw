# Sources shared by both apps
set(APP_COMMON_SRC
    MemMgr.cpp
    MemMgr.h
    YUVFileIO.cpp
    YUVFileIO.h
    ClockSync.cpp
    ClockSync.h
    LatencyStats.h
    SEIParser.h
    SEITimestamp.c
    SEITimestamp.h
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
    liballegro_app_static
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
    liballegro_decode_static
    liballegro_app_static
    asio
)
