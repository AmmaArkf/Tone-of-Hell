# Downloads the FluidR3 GM SoundFont: recordings of the 128 General MIDI instruments and a drum
# kit, by Frank Wen, released under the MIT license (see COPYING in the archive). It is too large
# for the repository (about 135 MB), so CMake fetches the original release from the Debian
# archive and checks its SHA-256. The programs find it in "sounds" next to themselves.
#
# Configure with -DTOH_DOWNLOAD_SOUNDS=OFF to build without it (instrument tracks then use the
# simple built-in synthesizers), or point FETCHCONTENT_SOURCE_DIR_FLUID_SOUNDFONT to an already
# unpacked copy.

option(TOH_DOWNLOAD_SOUNDS "Download the FluidR3 GM SoundFont (MIT, about 135 MB)" ON)

if(TOH_DOWNLOAD_SOUNDS)
    include(FetchContent)
    FetchContent_Declare(fluid_soundfont
        URL http://deb.debian.org/debian/pool/main/f/fluid-soundfont/fluid-soundfont_3.1.orig.tar.gz
        URL_HASH SHA256=2621acaa1c78e4abdb24bdd163230cc577e61276936d6aa6e3180582142f0343
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(fluid_soundfont)
endif()

# Copies the SoundFont and its license into "sounds" next to a program.
function(toh_copy_sounds target)
    if(TOH_DOWNLOAD_SOUNDS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:${target}>/sounds"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${fluid_soundfont_SOURCE_DIR}/FluidR3_GM.sf2"
                "$<TARGET_FILE_DIR:${target}>/sounds/FluidR3_GM.sf2"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${fluid_soundfont_SOURCE_DIR}/COPYING"
                "$<TARGET_FILE_DIR:${target}>/sounds/FluidR3_GM-LICENSE.txt"
            VERBATIM)
    endif()
endfunction()
