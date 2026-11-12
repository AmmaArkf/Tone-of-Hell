add_library(toh_app STATIC
    src/session.cpp)

target_include_directories(toh_app PUBLIC include)
target_link_libraries(toh_app
    PUBLIC toh_core toh_audio
    PRIVATE toh_warnings)
