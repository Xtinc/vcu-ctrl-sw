# lib_app — application helper layer (I/O, buffer pools, frame utilities)
#
# Produces:
#   liballegro_app        — shared library  (used at run-time by executables)
#   liballegro_app_static — static archive  (linked directly into executables)

set(LIB_APP_SRC
    utils.cpp
    convert.cpp
    BufPool.cpp
    PixMapBufPool.cpp
    AllocatorTracker.cpp
    YuvIO.cpp
    JsonFile.cpp
    FileUtils.cpp
    MD5.cpp
    Parser.cpp
    Tokenizer.cpp
    FrameReader.cpp
    UnCompFrameReader.cpp
    UnCompFrameWriter.cpp
    CompFrameCommon.cpp
    BaseFrameWriter.cpp
    SinkFrame.cpp
    SinkCrop.cpp
    SinkCrcDump.cpp
    SinkStreamMd5.cpp
    PlaneUtils.cpp
)

if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
    list(APPEND LIB_APP_SRC
        plateform_windows.cpp
        console_windows.cpp
    )
else()
    list(APPEND LIB_APP_SRC
        console_linux.cpp
        plateform_linux.cpp
    )
endif()

# Collect objects: lib_app sources + lib_common (app is a self-contained lib)
set(ALL_APP_OBJECTS
    ${LIB_APP_SRC}
    $<TARGET_OBJECTS:lib_common_obj>
    $<TARGET_OBJECTS:lib_rtos_obj>
)

add_library(liballegro_app        SHARED ${ALL_APP_OBJECTS})
add_library(liballegro_app_static STATIC ${ALL_APP_OBJECTS})

foreach(_tgt liballegro_app liballegro_app_static)
    target_link_libraries(${_tgt} PUBLIC Threads::Threads)
    set_target_properties(${_tgt} PROPERTIES OUTPUT_NAME allegro_app)
endforeach()

set_target_properties(liballegro_app_static PROPERTIES
    OUTPUT_NAME allegro_app
    SUFFIX      ".a"
)
