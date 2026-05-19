# Source layout: only three groups (common / encoder / decoder)
set(APP_COMMON_SRC
    ClockSync.cpp
    LatencyStats.cpp
    MemMgr.cpp
    YUVFileIO.cpp
)

set(APP_ENC_SRC
    DMAFd.h
    EncMgr.cpp
    EncMgr.h
    RTEncoder.cpp
    RTEncoder.h
    V4L2Source.cpp
    V4L2Source.h
    XilinxSyncIP.cpp
    XilinxSyncIP.h
    main_enc.cpp
)

set(APP_DEC_SRC
    DRMDisplay.cpp
    DRMDisplay.h
    RTDecoder.cpp
    RTDecoder.h
    SliceFeeder.cpp
    SliceFeeder.h
    main_dec.cpp
)

# Libraries shared by app executables
set(APP_COMMON_LIBS
    liballegro_encode_static
    liballegro_decode_static
    asio
)

# --- Encoder app ---
add_executable(vcu_enc
    ${APP_COMMON_SRC}
    ${APP_ENC_SRC}
)
target_link_libraries(vcu_enc PRIVATE ${APP_COMMON_LIBS})

# --- Decoder app ---
add_executable(vcu_dec
    ${APP_COMMON_SRC}
    ${APP_DEC_SRC}
)
target_link_libraries(vcu_dec PRIVATE ${APP_COMMON_LIBS})
target_include_directories(vcu_dec PRIVATE ${LIBDRM_INCLUDE_DIRS})
target_link_libraries(vcu_dec PRIVATE ${LIBDRM_LIBRARIES})

