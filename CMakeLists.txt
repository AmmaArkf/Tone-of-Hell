cmake_minimum_required(VERSION 3.20)

project(ToneOfHell
        VERSION 0.0.5
        DESCRIPTION "An open source digital audio workstation (DAW) in C++"
        LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TOH_BUILD_APPS "Build the command-line applications" ON)
option(TOH_BUILD_GUI_IMGUI "Build the Dear ImGui user interface (downloads SDL3 while configuring)" ON)
option(TOH_BUILD_TESTS "Build the unit tests" ON)
option(TOH_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# One place for the warning flags of our own code (third-party headers are marked SYSTEM).
add_library(toh_warnings INTERFACE)
if(MSVC)
    target_compile_options(toh_warnings INTERFACE /W4 /permissive-)
    if(TOH_WARNINGS_AS_ERRORS)
        target_compile_options(toh_warnings INTERFACE /WX)
    endif()
else()
    target_compile_options(toh_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion -Wnull-dereference)
    if(TOH_WARNINGS_AS_ERRORS)
        target_compile_options(toh_warnings INTERFACE -Werror)
    endif()
endif()

if(MSVC)
    add_compile_options(/utf-8)  # the sources contain translated texts in UTF-8
endif()

find_package(Threads REQUIRED)

# Layers, from the bottom up:
#   toh_core   the audio engine; no dependencies
#   toh_audio  sound devices and file decoding
#   toh_app    the session shared by every user interface
add_subdirectory(src/core)
add_subdirectory(src/audio)
add_subdirectory(src/app)

if(TOH_BUILD_APPS)
    add_subdirectory(apps/toh_player)
endif()

if(TOH_BUILD_GUI_IMGUI)
    add_subdirectory(apps/toh_imgui)
endif()

if(TOH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
