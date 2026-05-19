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

# --- Low-latency decoder with DRM/KMS display ---
# Requires libdrm (on ZYNQ: sudo apt-get install libdrm-dev)
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(LIBDRM libdrm)
endif()

if(LIBDRM_FOUND)
    add_executable(vcu_dec_display
        ${APP_COMMON_SRC}
        main_dec.cpp          # reuse same entry point; display is activated via config
        RTDecoder.cpp
        RTDecoder.h
        DRMDisplay.cpp
        DRMDisplay.h
        SliceFeeder.cpp
        SliceFeeder.h
    )
    target_compile_definitions(vcu_dec_display PRIVATE WITH_DRM_DISPLAY)
    target_include_directories(vcu_dec_display PRIVATE ${LIBDRM_INCLUDE_DIRS})
    target_link_libraries(vcu_dec_display
        PRIVATE
        liballegro_encode_static
        liballegro_decode_static
        asio
        ${LIBDRM_LIBRARIES}
    )
    message(STATUS "DRM display target 'vcu_dec_display' enabled (libdrm found)")
else()
    message(STATUS "libdrm not found — skipping 'vcu_dec_display' target. "
                   "Install libdrm-dev to enable DRM/KMS zero-copy display.")
endif()

