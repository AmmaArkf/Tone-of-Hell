add_executable(toh_tests
    test_main.cpp
    test_engine.cpp
    test_join.cpp
    test_latency.cpp
    test_midi.cpp
    test_mixer.cpp
    test_mp3.cpp
    test_notes.cpp
    test_plugins.cpp
    test_project.cpp
    test_range.cpp
    test_recording.cpp
    test_regions.cpp
    test_sampler.cpp
    test_session.cpp
    test_settings.cpp
    test_soundfont.cpp
    test_spsc_queue.cpp
    test_streaming.cpp
    test_transport.cpp
    test_undo.cpp)

target_include_directories(toh_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(toh_tests PRIVATE toh_core toh_app toh_miniz toh_warnings Threads::Threads)

toh_copy_runtime_dlls(toh_tests)

# The plugin tests load the example plugins from where they are built.
add_dependencies(toh_tests toh_example_plugins)
target_compile_definitions(toh_tests PRIVATE TOH_EXAMPLE_PLUGINS="$<TARGET_FILE:toh_example_plugins>")

add_test(NAME toh_tests COMMAND toh_tests)
