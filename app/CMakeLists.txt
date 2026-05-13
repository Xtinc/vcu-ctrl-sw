# Sources shared by both apps
set(APP_COMMON_SRC
    ClockSync.cpp
    LatencyStats.cpp
    MemMgr.cpp
    YUVFileIO.cpp
)

# --- Encoder app ---
add_executable(vcu_enc
    ${APP_COMMON_SRC}
    DMAFd.h
    EncMgr.cpp
    EncMgr.h
    main_enc.cpp
    RTEncoder.cpp
    RTEncoder.h
    V4L2Source.cpp
    V4L2Source.h
    XilinxSyncIP.cpp
    XilinxSyncIP.h
)
target_link_libraries(vcu_enc
    PRIVATE
    liballegro_encode_static
    liballegro_decode_static
    asio
)

# --- Decoder-to-YUV app ---
add_executable(vcu_dec
    ${APP_COMMON_SRC}
    main_dec.cpp
    RTDecoder.cpp
    RTDecoder.h
    SliceFeeder.cpp
    SliceFeeder.h
)
target_link_libraries(vcu_dec
    PRIVATE
    liballegro_encode_static
    liballegro_decode_static
    asio
)
