# Warning flags by compiler family
if(MSVC)
    add_compile_options(
        /W3
    )
else()
    # From codec_defs.mk: suppress third-party noise first, then re-enable
    # project-level checks.
    add_compile_options(
        -Wwrite-strings
        -Wredundant-decls
        -pedantic
        -pedantic-errors
        -Werror=pedantic
        -Wall
        -Wextra
    )
endif()

# 64-bit address support
if(ENABLE_64BIT)
    add_compile_definitions(AL_ENABLE_64BIT=1)
else()
    add_compile_definitions(AL_ENABLE_64BIT=0)
    # Force 32-bit on x86_64 hosts for GCC/Clang-like toolchains.
    # MSVC target bitness is selected by generator platform.
    if((CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang") AND CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
        add_compile_options(-m32)
        add_link_options(-m32)
    endif()
endif()