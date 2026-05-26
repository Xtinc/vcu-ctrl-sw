# --- Encoder v4l2 app ---
add_executable(vcu_enc_v4l2
    MemMgr.cpp
    EncMgr.cpp
    RTEncoder.cpp
    XilinxSyncIP.cpp
    V4L2Source.cpp
    main_enc_v4l2.cpp
    $<TARGET_OBJECTS:lib_network_obj>
)
target_link_libraries(vcu_enc_v4l2 PRIVATE liballegro_encode_static)

add_executable(vcu_dec_drm
    MemMgr.cpp
    DecMgr.cpp
    DRMDisplay.cpp
    RTDecoder.cpp
    main_dec_drm.cpp
    $<TARGET_OBJECTS:lib_network_obj>
)
target_link_libraries(vcu_dec_drm PRIVATE liballegro_decode_static ${LIBDRM_LIBRARIES})
target_include_directories(vcu_dec_drm PRIVATE ${LIBDRM_INCLUDE_DIRS})

