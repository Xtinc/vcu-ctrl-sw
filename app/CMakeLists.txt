# --- Encoder v4l2 app ---
add_executable(vcu_enc_v4l2
    MemMgr.cpp
    EncMgr.cpp
    RTEncoder.cpp
    XilinxSyncIP.cpp
    V4L2Source.cpp
    main_enc_v4l2.cpp
)
target_link_libraries(vcu_enc_v4l2 PRIVATE liballegro_encode_static lib_network_obj)

add_executable(vcu_dec_drm
    MemMgr.cpp
    DecMgr.cpp
    DRMDisplay.cpp
    RTDecoder.cpp
    main_dec_drm.cpp
)
target_link_libraries(vcu_dec_drm PRIVATE liballegro_decode_static lib_network_obj ${LIBDRM_LIBRARIES})
target_include_directories(vcu_dec_drm PRIVATE ${LIBDRM_INCLUDE_DIRS})

