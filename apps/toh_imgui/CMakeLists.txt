# The Dear ImGui user interface. SDL3 provides the window, input and file dialogs.

include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)  # extracted files get the time of extraction
endif()

# SDL3 is too large to keep in the repository, so the build downloads the official release
# archive once and checks it against a fixed checksum.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_AUDIO OFF CACHE BOOL "" FORCE)  # sound goes through miniaudio
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.20/SDL3-3.4.20.tar.gz
    URL_HASH SHA256=f3bb06f55bed9557227a700af2ea62e49497ebdf58037732a731a5fb3ef8581f)
FetchContent_MakeAvailable(SDL3)

set(IMGUI_DIR ${PROJECT_SOURCE_DIR}/third_party/imgui)
add_library(toh_imgui_lib STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp
    ${IMGUI_DIR}/backends/imgui_impl_sdlrenderer3.cpp)
target_include_directories(toh_imgui_lib SYSTEM PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
target_link_libraries(toh_imgui_lib PUBLIC SDL3::SDL3)

add_executable(toh_imgui WIN32 main.cpp)
target_link_libraries(toh_imgui PRIVATE toh_app toh_imgui_lib toh_warnings)
