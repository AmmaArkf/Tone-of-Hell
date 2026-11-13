# Hosting of plugins: CLAP for now. The CLAP headers stay private to this library.
add_library(toh_plugins STATIC src/clap_plugin.cpp)
target_include_directories(toh_plugins PUBLIC include)
target_include_directories(toh_plugins SYSTEM PRIVATE ${clap_SOURCE_DIR}/include)
target_link_libraries(toh_plugins
    PUBLIC toh_core
    PRIVATE toh_warnings ${CMAKE_DL_LIBS})
