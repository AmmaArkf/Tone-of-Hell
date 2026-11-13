add_executable(toh_player main.cpp)
target_link_libraries(toh_player PRIVATE toh_core toh_audio toh_warnings)
toh_copy_runtime_dlls(toh_player)
