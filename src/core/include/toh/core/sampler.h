#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "toh/core/notes.h"

namespace toh::core {

/// The spectrum kept for drawing: kSpectrumBins levels (0 = 60 dB below the loudest, 1 = the
/// loudest) on a logarithmic frequency scale from kSpectrumLow to kSpectrumHigh Hz.
constexpr std::size_t kSpectrumBins = 256;
constexpr double kSpectrumLow = 30.0;
constexpr double kSpectrumHigh = 8000.0;

/// How long a sampler note fades out after it ends.
constexpr double kSamplerReleaseSeconds = 0.12;

/// What analyze_pitch() finds in a sound.
struct PitchAnalysis {
    double frequency = 0.0;  ///< Hz of the fundamental of the loudest tone, 0 when none
    double note = -1.0;      ///< the same as a MIDI note (69 = A 440 Hz, with a fraction), or -1
    std::vector<float> spectrum;  ///< see kSpectrumBins
};

/// Finds the note of a sound from its spectrum: the loudest frequency of its loudest part, or
/// the fundamental below it when that frequency is one of its overtones.
PitchAnalysis analyze_pitch(const float* mono, std::size_t frames, double sample_rate);

/// A sound that plays notes: at `root` it plays as recorded, every semitone above or below
/// plays it 2^(1/12) times faster or slower (like a tape), up to the end of the sound.
struct SamplerSound {
    std::string name;
    std::shared_ptr<const std::vector<float>> data;  ///< mono samples
    double sample_rate = 48000.0;
    double root = 60.0;       ///< the note at which it plays unchanged (may have a fraction)
    double detected = -1.0;   ///< the note analyze_pitch() found, or -1
    double frequency = 0.0;   ///< its frequency in Hz, or 0
    std::vector<float> spectrum;
};

/// Builds a sampler sound and analyses it. A `root` below 0 means the note that was found
/// (60 when none was found).
std::shared_ptr<const SamplerSound> make_sampler_sound(std::string name, std::vector<float> mono,
                                                       double sample_rate, double root = -1.0);

/// The sound of one note `t_frames` frames after it started (also during the release), like
/// note_sample(). Real-time safe.
float sampler_note(const SamplerSound& sound, const Note& note, double t_frames,
                   double sample_rate) noexcept;

}  // namespace toh::core
