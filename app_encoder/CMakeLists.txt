aux_source_directory(. RT_VCU_SRC)
add_executable(vcu_enc ${RT_VCU_SRC})
target_link_libraries(
    vcu_enc
    PRIVATE
    liballegro_encode_static
    liballegro_app_static
)
