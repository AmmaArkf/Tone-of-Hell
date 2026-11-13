# miniz (third_party/miniz) reads and writes the zip archives that projects are saved in.
add_library(toh_miniz STATIC ${PROJECT_SOURCE_DIR}/third_party/miniz/miniz.c)
target_include_directories(toh_miniz SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/miniz)
if(MSVC)
    target_compile_definitions(toh_miniz PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

add_library(toh_app STATIC
    src/build_info.cpp
    src/i18n.cpp
    src/log.cpp
    src/midi_file.cpp
    src/midi_input.cpp
    src/session.cpp
    src/settings.cpp)

target_include_directories(toh_app PUBLIC include)
target_link_libraries(toh_app
    PUBLIC toh_core toh_audio Threads::Threads
    PRIVATE toh_warnings toh_miniz)
if(WIN32)
    target_link_libraries(toh_app PRIVATE winmm)  # MIDI keyboards
endif()

# Build information shown in the user interfaces. The commit is read when CMake configures the
# project, so after new commits it is refreshed by the next configure run.
set(TOH_GIT_COMMIT "unknown")
find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(
        COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
        WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
        OUTPUT_VARIABLE TOH_GIT_OUTPUT
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE TOH_GIT_RESULT)
    if(TOH_GIT_RESULT EQUAL 0 AND TOH_GIT_OUTPUT)
        set(TOH_GIT_COMMIT "${TOH_GIT_OUTPUT}")
    endif()
endif()
set_source_files_properties(src/build_info.cpp PROPERTIES
    COMPILE_DEFINITIONS "TOH_VERSION=\"${PROJECT_VERSION}\";TOH_GIT_COMMIT=\"${TOH_GIT_COMMIT}\"")
