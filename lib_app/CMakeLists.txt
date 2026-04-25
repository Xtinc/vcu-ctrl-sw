# lib_app — application helper layer (I/O, buffer pools, frame utilities)
#
# Produces:
#   liballegro_app        — shared library  (used at run-time by executables)
#   liballegro_app_static — static archive  (linked directly into executables)

set(LIB_APP_SRC
    AllocatorTracker.cpp
    BaseFrameWriter.cpp
    BufPool.cpp
    CompFrameCommon.cpp
    convert.cpp
    FileUtils.cpp
    FrameReader.cpp
    JsonFile.cpp
    MD5.cpp
    Parser.cpp
    PixMapBufPool.cpp
    PlaneUtils.cpp
    SinkCrcDump.cpp
    SinkCrop.cpp
    SinkFrame.cpp
    SinkStreamMd5.cpp
    Tokenizer.cpp
    UnCompFrameReader.cpp
    UnCompFrameWriter.cpp
    utils.cpp
    YuvIO.cpp
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
