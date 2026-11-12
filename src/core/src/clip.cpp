#include "toh/core/clip.h"

#include <cmath>

namespace toh::core {

std::shared_ptr<const ClipData> make_sine_clip(double frequency_hz, double seconds,
                                               std::uint32_t sample_rate, std::uint32_t channels,
                                               float amplitude) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = channels;
    clip->sample_rate = sample_rate;

    const auto frames = static_cast<std::size_t>(std::llround(seconds * sample_rate));
    clip->samples.resize(frames * channels);

    constexpr double kTwoPi = 6.283185307179586476925286766559;
    const double step = kTwoPi * frequency_hz / static_cast<double>(sample_rate);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto value = static_cast<float>(static_cast<double>(amplitude) *
                                              std::sin(step * static_cast<double>(i)));
        for (std::size_t c = 0; c < channels; ++c) {
            clip->samples[i * channels + c] = value;
        }
    }
    return clip;
}

}  // namespace toh::core
