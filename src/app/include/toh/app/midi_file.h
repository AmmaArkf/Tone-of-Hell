#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "toh/core/notes.h"

namespace toh::app {

/// The notes of one part of a Standard MIDI File: one channel of one track.
struct MidiPart {
    std::string name;      // UTF-8 as far as the file says; may be empty
    int channel = 0;       // 0..15; channel 9 (the tenth) holds drums
    int program = 0;       // 0..127, or core::SoundFont::kDrums for channel 9
    std::vector<core::Note> notes;  // in frames from the start of the song, sorted by start
};

/// A song as far as Tone of Hell uses it: tempo, time signature and the notes.
struct MidiSong {
    double bpm = 120.0;  // the first tempo of the file
    int beats_per_bar = 4;
    int beat_unit = 4;
    std::vector<MidiPart> parts;
    std::int64_t frames = 0;  // where the last note ends
};

/// Reads a Standard MIDI File (format 0 or 1). Times follow every tempo change of the file and
/// are converted to frames at `sample_rate`. Returns false (and `error`) when the file cannot
/// be read.
bool read_midi_file(const std::filesystem::path& path, std::uint32_t sample_rate, MidiSong& song,
                    std::string* error = nullptr);
/// The same from bytes in memory.
bool parse_midi(const std::vector<unsigned char>& bytes, std::uint32_t sample_rate, MidiSong& song,
                std::string* error = nullptr);

/// Writes a Standard MIDI File (format 1, 480 ticks per quarter note) with one tempo: every
/// part becomes a track on its channel.
bool write_midi_file(const std::filesystem::path& path, const MidiSong& song,
                     std::uint32_t sample_rate, std::string* error = nullptr);

}  // namespace toh::app
