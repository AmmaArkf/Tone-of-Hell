#include "toh/core/clip.h"

#include <algorithm>
#include <cmath>

namespace toh::core {

std::size_t read_clip_frames(const ClipData& clip, std::int64_t first, std::size_t count,
                             float* out) {
    if (first < 0 || clip.channels == 0) {
        return 0;
    }
    if (clip.disk != nullptr) {
        return clip.disk->read_blocking(first, count, out);
    }
    const std::int64_t frames = clip.frames();
    if (first >= frames) {
        return 0;
    }
    const auto n = static_cast<std::size_t>(
        std::min<std::int64_t>(static_cast<std::int64_t>(count), frames - first));
    std::copy_n(clip.samples.data() + static_cast<std::size_t>(first) * clip.channels,
                n * clip.channels, out);
    return n;
}

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
