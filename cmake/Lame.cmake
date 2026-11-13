# Builds the LAME MP3 encoder as a separate shared library (mp3lame).
#
# LAME is licensed under the LGPL. It is kept out of this repository and out of our own
# binaries: CMake downloads the official release archive (checked against a fixed SHA-256) and
# builds it as its own DLL / shared object, which users may replace with another build of
# LAME. Our code stays under the MIT license. See third_party/README.md.

include(FetchContent)

FetchContent_Declare(lame
    URL https://downloads.sourceforge.net/project/lame/lame/4.0/lame-4.0.tar.gz
    URL_HASH SHA256=3df5124d5ad3a98312ffd7ba6a9b36230e4f8a3e66d3ce0f425e336c32d216eb
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
# LAME has no CMakeLists.txt, so this only downloads and unpacks it.
FetchContent_MakeAvailable(lame)

set(TOH_LAME_SOURCES
    bitstream.c encoder.c fft.c gain_analysis.c id3tag.c lame.c
    newmdct.c presets.c psymodel.c quantize.c quantize_pvt.c reservoir.c set_get.c
    tables.c takehiro.c util.c vbrquantize.c VbrTag.c version.c)
# mpglib_interface.c is left out: it is the bridge to the decoder, which is not built.
list(TRANSFORM TOH_LAME_SOURCES PREPEND "${lame_SOURCE_DIR}/libmp3lame/")

add_library(mp3lame SHARED ${TOH_LAME_SOURCES})
target_include_directories(mp3lame
    PUBLIC "${lame_SOURCE_DIR}/include"
    PRIVATE "${lame_SOURCE_DIR}/libmp3lame" "${CMAKE_CURRENT_LIST_DIR}/lame")
target_compile_definitions(mp3lame PRIVATE HAVE_CONFIG_H)
set_target_properties(mp3lame PROPERTIES
    OUTPUT_NAME mp3lame
    C_STANDARD 99
    C_EXTENSIONS ON
    WINDOWS_EXPORT_ALL_SYMBOLS ON)

# Third-party code: no warnings in our build output, and tolerate the older C it is written in.
if(MSVC)
    target_compile_options(mp3lame PRIVATE /w)
    target_compile_definitions(mp3lame PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(mp3lame PRIVATE -w
        -Wno-error=implicit-function-declaration
        -Wno-error=incompatible-pointer-types
        -Wno-error=int-conversion)
endif()

if(UNIX)
    target_link_libraries(mp3lame PRIVATE m)
endif()
