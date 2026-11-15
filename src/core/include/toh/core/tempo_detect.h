#pragma once

#include <cstddef>

namespace toh::core {

/// The tempo found in a piece of audio.
struct TempoEstimate {
    double bpm = 0.0;         ///< beats per minute, 0 when no steady beat was found
    double first_beat = 0.0;  ///< the frame of the first beat (0 .. one beat)
    double confidence = 0.0;  ///< how clearly the beat stands out (about 1 = not at all)
};

/// Finds the tempo of interleaved audio from its onsets (where the sound gets suddenly louder):
/// the period at which they repeat most strongly, between 60 and 200 BPM, and where the beats
/// fall. Uses at most the first two minutes. Not real-time safe.
TempoEstimate detect_tempo(const float* interleaved, std::size_t frames, std::size_t channels,
                           double sample_rate);

}  // namespace toh::core
