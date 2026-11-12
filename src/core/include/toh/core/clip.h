#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace toh::core {

/// A block of audio held completely in memory: interleaved 32-bit float samples.
///
/// A clip is immutable once it has been handed to the engine, so the audio thread can read it
/// without any locking.
struct ClipData {
    std::vector<float> samples;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;

    /// Number of frames (one frame holds one sample for every channel).
    std::int64_t frames() const noexcept {
        return channels == 0 ? 0 : static_cast<std::int64_t>(samples.size() / channels);
    }
};

/// Generates a sine tone. When `seconds * frequency_hz` is a whole number the clip loops
/// without a click. Not real-time safe (it allocates).
std::shared_ptr<const ClipData> make_sine_clip(double frequency_hz, double seconds,
                                               std::uint32_t sample_rate, std::uint32_t channels,
                                               float amplitude = 0.25f);

}  // namespace toh::core
