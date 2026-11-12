add_executable(toh_tests
    test_main.cpp
    test_engine.cpp
    test_mixer.cpp
    test_recording.cpp
    test_regions.cpp
    test_session.cpp
    test_spsc_queue.cpp
    test_transport.cpp)

target_include_directories(toh_tests SYSTEM PRIVATE ${PROJECT_SOURCE_DIR}/third_party/doctest)
target_link_libraries(toh_tests PRIVATE toh_core toh_app toh_warnings Threads::Threads)

add_test(NAME toh_tests COMMAND toh_tests)
