aux_source_directory(. RT_VCU_DEC_SRC)
add_executable(vcu_dec ${RT_VCU_DEC_SRC})
target_link_libraries(
    vcu_dec
    PRIVATE
    liballegro_decode_static
    liballegro_app_static
)
