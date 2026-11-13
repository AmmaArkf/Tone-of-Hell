# The VST3 plugin interfaces (https://github.com/steinbergmedia/vst3_pluginterfaces, MIT license
# since VST 3.8): the C++ interfaces a host needs, without the rest of the SDK. CMake fetches the
# release archive once and checks its SHA-256. Point FETCHCONTENT_SOURCE_DIR_VST3_PLUGINTERFACES
# to an unpacked copy to build without network.

include(FetchContent)
FetchContent_Declare(vst3_pluginterfaces
    URL https://github.com/steinbergmedia/vst3_pluginterfaces/archive/refs/tags/v3.8.1_build_84.tar.gz
    URL_HASH SHA256=19e88e225b6ecb4a3c429d3ff2fd511be9c6c8b8b1c7e86b05949b45c0553e2d
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR no-cmake)  # only the files; their own CMake project needs the whole SDK
FetchContent_MakeAvailable(vst3_pluginterfaces)

# The headers include each other as "pluginterfaces/...", so they get that folder name.
set(TOH_VST3_INCLUDE ${CMAKE_BINARY_DIR}/vst3_include)
file(COPY ${vst3_pluginterfaces_SOURCE_DIR}/ DESTINATION ${TOH_VST3_INCLUDE}/pluginterfaces
     PATTERN ".git" EXCLUDE)

add_library(toh_vst3_interfaces STATIC
    ${TOH_VST3_INCLUDE}/pluginterfaces/base/funknown.cpp
    ${TOH_VST3_INCLUDE}/pluginterfaces/base/coreiids.cpp
    ${PROJECT_SOURCE_DIR}/src/plugins/src/vst3_iids.cpp)
target_include_directories(toh_vst3_interfaces SYSTEM PUBLIC ${TOH_VST3_INCLUDE})
if(WIN32)
    target_link_libraries(toh_vst3_interfaces PUBLIC ole32)
endif()
