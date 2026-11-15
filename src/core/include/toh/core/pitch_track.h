#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace toh::core {

/// The pitch of a sung or played line along its time, one value per hop of `hop` frames.
struct PitchCurve {
    std::size_t hop = 512;
    std::vector<float> midi;  ///< the pitch as a MIDI note number with fractions, or -1 where nothing is sung
};

/// A note found in a pitch curve: where it begins and how long it lasts (in frames of the
/// sound), and its pitch (the median, with fractions).
struct PitchNote {
    std::int64_t start = 0;
    std::int64_t length = 0;
    float pitch = 60.0f;
};

/// Follows the pitch of one voice or instrument (60 Hz to 1 kHz) with the YIN method on a
/// copy at a quarter of the sample rate. Chords and noise give gaps. Not real-time safe.
PitchCurve track_pitch(const float* interleaved, std::size_t frames, std::size_t channels, double sample_rate);

/// Cuts a pitch curve into notes: a new note begins where the pitch jumps by more than half a
/// semitone and stays there, or after a gap. Very short notes are left out.
std::vector<PitchNote> find_pitch_notes(const PitchCurve& curve);

}  // namespace toh::core
