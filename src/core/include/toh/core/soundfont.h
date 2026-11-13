#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "toh/core/notes.h"

namespace toh::core {

/// Recorded instruments from a SoundFont 2 file (.sf2): the samples of real instruments with
/// the rules for playing them at every pitch and loudness. Notes are rendered from it the same
/// way as by the built-in synthesizers: as a pure function of the time since the note started,
/// so seeks, loops, reverse playback and exports all sound the same.
///
/// A loaded SoundFont never changes, so the audio thread reads it without locking.
class SoundFont {
public:
    /// Program numbers: 0 to 127 are the General MIDI instruments (bank 0), kDrums is the
    /// standard drum kit (bank 128, program 0).
    static constexpr int kDrums = 128;

    struct Zone {
        std::uint8_t key_low = 0;
        std::uint8_t key_high = 127;
        std::uint8_t velocity_low = 0;
        std::uint8_t velocity_high = 127;
        std::uint32_t start = 0;       // in data(): first frame of the sample
        std::uint32_t end = 0;         // one past its last frame
        std::uint32_t loop_start = 0;
        std::uint32_t loop_end = 0;
        bool loop = false;
        double root_key = 60.0;        // the key at which the sample plays unchanged
        double sample_rate = 44100.0;
        float gain = 1.0f;
        float left = 1.0f;             // pan as the gain of each side
        float right = 1.0f;
        double attack = 0.001;         // seconds
        double hold = 0.0;
        double decay = 0.001;
        double sustain = 1.0;          // level
        double release = 0.01;
    };

    struct Preset {
        std::string name;
        int bank = 0;
        int program = 0;
        std::vector<Zone> zones;
        std::vector<std::vector<std::uint16_t>> by_key;  // for every key: the zones that cover it
        double release = 0.0;  // the longest release of its zones, in seconds
    };

    /// Reads a .sf2 file. Returns null when it cannot be read; `error` says why.
    static std::shared_ptr<const SoundFont> load(const std::filesystem::path& path,
                                                 std::string* error = nullptr);
    /// Reads a SoundFont from memory (used by tests).
    static std::shared_ptr<const SoundFont> parse(const std::vector<char>& bytes,
                                                  std::string* error = nullptr);

    /// The preset for a program (see kDrums), or null when the file has none.
    const Preset* find(int program) const noexcept;
    const std::vector<Preset>& presets() const noexcept { return presets_; }
    const std::filesystem::path& path() const noexcept { return path_; }

    /// Adds one note's sound `t_frames` frames after it started to `left` and `right`.
    void add_note(const Preset& preset, const Note& note, double t_frames, double sample_rate,
                  float& left, float& right) const noexcept;

private:
    std::vector<std::int16_t> data_;
    std::vector<Preset> presets_;
    std::map<int, std::size_t> by_program_;
    std::filesystem::path path_;
};

}  // namespace toh::core
