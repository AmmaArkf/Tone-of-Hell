# Example CLAP plugins (Echo, Tremolo, Gain, Simple Synth, Noise Gate, Denoiser) that come with the
# program, in one file:
# ToneOfHell-Examples.clap. The program finds them in "plugins" next to itself.
add_library(toh_example_plugins MODULE toh_examples.cpp)
target_include_directories(toh_example_plugins SYSTEM PRIVATE ${clap_SOURCE_DIR}/include)
target_link_libraries(toh_example_plugins PRIVATE toh_warnings)
set_target_properties(toh_example_plugins PROPERTIES
    PREFIX ""
    SUFFIX ".clap"
    OUTPUT_NAME "ToneOfHell-Examples")
if(MINGW)
    # The plugin must not need the MinGW runtime DLLs next to the program that loads it.
    target_link_options(toh_example_plugins PRIVATE -static-libgcc -static-libstdc++)
endif()

# Copies the example plugins into "plugins" next to a program.
function(toh_copy_plugins target)
    add_dependencies(${target} toh_example_plugins)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/plugins"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:toh_example_plugins>"
                "$<TARGET_FILE_DIR:${target}>/plugins/"
        VERBATIM)
endfunction()
