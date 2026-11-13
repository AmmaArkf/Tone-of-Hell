add_executable(toh_tests
    test_main.cpp
    test_engine.cpp
    test_join.cpp
    test_mixer.cpp
    test_mp3.cpp
    test_project.cpp
    test_recording.cpp
    test_regions.cpp
    test_session.cpp
    test_settings.cpp
    test_spsc_queue.cpp
    test_streaming.cpp
    test_transport.cpp)

target_include_directories(toh_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(toh_tests PRIVATE toh_core toh_app toh_miniz toh_warnings Threads::Threads)

toh_copy_runtime_dlls(toh_tests)

add_test(NAME toh_tests COMMAND toh_tests)
