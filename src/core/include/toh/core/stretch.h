#pragma once

#include <cstddef>
#include <vector>

namespace toh::core {

/// Makes interleaved audio `factor` times as long without changing its pitch (WSOLA: pieces of
/// the sound are laid over each other where they match best). `factor` 0.25 to 4.
/// Not real-time safe.
std::vector<float> time_stretch(const float* interleaved, std::size_t frames, std::size_t channels,
                                double factor);

/// Moves the pitch of interleaved audio by `semitones` (-24 to 24) and keeps its length.
std::vector<float> pitch_shift(const float* interleaved, std::size_t frames, std::size_t channels,
                               double semitones);

}  // namespace toh::core
