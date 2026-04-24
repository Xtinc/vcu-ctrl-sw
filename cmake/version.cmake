# Retrieve SCM revision and branch from git (used by exe_encoder / exe_decoder)
find_package(Git QUIET)

if(Git_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_REV
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --abbrev-ref HEAD
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        OUTPUT_VARIABLE GIT_BRANCH
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
endif()

if(NOT GIT_REV)
    set(GIT_REV "0")
endif()
if(NOT GIT_BRANCH)
    set(GIT_BRANCH "unknown")
endif()

# These match SCM_REV_SW / SCM_BRANCH / DELIVERY_* in the original Makefile
set(SCM_REV_SW    "${GIT_REV}"   CACHE STRING "SCM revision (git HEAD)")
set(SCM_BRANCH    "${GIT_BRANCH}" CACHE STRING "SCM branch")
set(DELIVERY_BUILD_NUMBER "0"       CACHE STRING "Delivery build number")
set(DELIVERY_SCM_REV      "unknown" CACHE STRING "Delivery SCM revision")
set(DELIVERY_DATE         "unknown" CACHE STRING "Delivery date")
