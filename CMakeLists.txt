
# re_cherry - ReXGlue Recompiled Project
#
# This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(re_cherry LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

# Sources
set(RECHERRY_SOURCES
    src/main.cpp
    src/hooks.cpp
)

if(WIN32)
    add_executable(re_cherry WIN32 ${RECHERRY_SOURCES})
else()
    add_executable(re_cherry ${RECHERRY_SOURCES})
endif()

rexglue_setup_target(re_cherry)
