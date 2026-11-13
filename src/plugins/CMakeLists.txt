# Hosting of plugins: CLAP and VST3. Their headers stay private to this library.
add_library(toh_plugins STATIC src/clap_plugin.cpp src/vst3_plugin.cpp)
target_include_directories(toh_plugins PUBLIC include)
target_include_directories(toh_plugins SYSTEM PRIVATE ${clap_SOURCE_DIR}/include)
target_link_libraries(toh_plugins
    PUBLIC toh_core
    PRIVATE toh_warnings toh_vst3_interfaces ${CMAKE_DL_LIBS})
