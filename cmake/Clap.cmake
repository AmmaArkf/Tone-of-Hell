# The CLAP plugin standard (https://github.com/free-audio/clap, MIT license): C headers that say
# how a host and a plugin talk to each other. CMake fetches the release archive once and checks
# its SHA-256. Point FETCHCONTENT_SOURCE_DIR_CLAP to an unpacked copy to build without network.

include(FetchContent)
FetchContent_Declare(clap
    URL https://github.com/free-audio/clap/archive/refs/tags/1.2.2.tar.gz
    URL_HASH SHA256=1472bfe3b9f4eac3d7aca57d4a528f0c39709a2af82900253ce6a5b86e0b60e1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(clap)
