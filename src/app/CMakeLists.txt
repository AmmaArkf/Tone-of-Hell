add_library(toh_app STATIC
    src/build_info.cpp
    src/session.cpp)

target_include_directories(toh_app PUBLIC include)
target_link_libraries(toh_app
    PUBLIC toh_core toh_audio
    PRIVATE toh_warnings)

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
