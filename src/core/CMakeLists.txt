add_library(toh_core STATIC
    src/clip.cpp
    src/engine.cpp
    src/paged_file.cpp
    src/synth.cpp)

target_include_directories(toh_core PUBLIC include)
target_link_libraries(toh_core PRIVATE toh_warnings)
