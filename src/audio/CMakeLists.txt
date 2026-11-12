add_library(toh_audio STATIC
    src/audio_device.cpp
    src/clip_loader.cpp
    src/miniaudio_impl.cpp
    src/wav_writer.cpp)

target_include_directories(toh_audio PUBLIC include)
# miniaudio is a third-party header: SYSTEM keeps its warnings out of our build output.
target_include_directories(toh_audio SYSTEM PUBLIC ${PROJECT_SOURCE_DIR}/third_party/miniaudio)
target_link_libraries(toh_audio
    PUBLIC toh_core
    PRIVATE toh_warnings Threads::Threads ${CMAKE_DL_LIBS})
