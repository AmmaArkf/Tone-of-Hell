add_library(toh_core STATIC
    src/clip.cpp
    src/engine.cpp
    src/paged_file.cpp
    src/sampler.cpp
    src/soundfont.cpp
    src/stock_sounds.cpp
    src/stretch.cpp
    src/synth.cpp
    src/tempo_detect.cpp)

target_include_directories(toh_core PUBLIC include)
find_package(Threads REQUIRED)
target_link_libraries(toh_core PUBLIC Threads::Threads)
target_link_libraries(toh_core PRIVATE toh_warnings)
