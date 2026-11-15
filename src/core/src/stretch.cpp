#include "toh/core/stretch.h"

#include <algorithm>
#include <cmath>

namespace toh::core {
namespace {

constexpr std::size_t kWindow = 2048;            // frames in one piece
constexpr std::size_t kHop = kWindow / 2;        // the pieces of the result overlap by half
constexpr long kTolerance = 512;                 // how far a piece may move to match
constexpr std::size_t kDecimate = 4;             // the matching looks at every 4th frame

// How well the sound at `candidate` continues the sound at `target` (normalized correlation
// over one hop, on every kDecimate-th frame of the mono mix).
double match(const std::vector<float>& mono, long target, long candidate) {
    const long size = static_cast<long>(mono.size());
    double product = 0.0;
    double energy = 1e-9;
    for (long n = 0; n < static_cast<long>(kHop); n += static_cast<long>(kDecimate)) {
        const long a = target + n;
        const long b = candidate + n;
        if (a >= size || b >= size || a < 0 || b < 0) {
            break;
        }
        product += static_cast<double>(mono[static_cast<std::size_t>(a)]) * mono[static_cast<std::size_t>(b)];
        energy += static_cast<double>(mono[static_cast<std::size_t>(b)]) * mono[static_cast<std::size_t>(b)];
    }
    return product / std::sqrt(energy);
}

}  // namespace

std::vector<float> time_stretch(const float* interleaved, std::size_t frames, std::size_t channels,
                                double factor) {
    if (interleaved == nullptr || frames == 0 || channels == 0 || !(factor > 0.0)) {
        return {};
    }
    factor = std::clamp(factor, 0.25, 4.0);
    const auto out_frames = static_cast<std::size_t>(std::llround(static_cast<double>(frames) * factor));
    std::vector<float> mono(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (std::size_t c = 0; c < channels; ++c) {
            sum += interleaved[i * channels + c];
        }
        mono[i] = sum / static_cast<float>(channels);
    }
    std::vector<float> window(kWindow);
    for (std::size_t n = 0; n < kWindow; ++n) {
        window[n] = static_cast<float>(0.5 - 0.5 * std::cos(2.0 * 3.14159265358979 * static_cast<double>(n) /
                                                            static_cast<double>(kWindow)));
    }
    std::vector<float> out((out_frames + kWindow) * channels, 0.0f);
    std::vector<float> weight(out_frames + kWindow, 0.0f);
    const double input_hop = static_cast<double>(kHop) / factor;
    const long last_start = static_cast<long>(frames) - 1;
    long previous = 0;
    for (std::size_t k = 0;; ++k) {
        const std::size_t out_pos = k * kHop;
        if (out_pos >= out_frames) {
            break;
        }
        long start = std::lround(static_cast<double>(k) * input_hop);
        if (k > 0) {
            // The piece that would continue the previous one naturally, and the place near the
            // nominal one that sounds most like it.
            const long target = previous + static_cast<long>(kHop);
            long best = start;
            double best_match = -1e30;
            for (long d = -kTolerance; d <= kTolerance; d += 8) {
                const double m = match(mono, target, start + d);
                if (m > best_match) {
                    best_match = m;
                    best = start + d;
                }
            }
            for (long d = -7; d <= 7; ++d) {
                const double m = match(mono, target, best + d);
                if (m > best_match) {
                    best_match = m;
                    best += d;
                    d = -8;  // look around the new best place again
                }
            }
            start = best;
        }
        start = std::clamp(start, 0L, std::max(last_start, 0L));
        for (std::size_t n = 0; n < kWindow; ++n) {
            const std::size_t from = static_cast<std::size_t>(start) + n;
            if (from >= frames) {
                break;
            }
            const float w = window[n];
            const float* src = interleaved + from * channels;
            float* dst = out.data() + (out_pos + n) * channels;
            for (std::size_t c = 0; c < channels; ++c) {
                dst[c] += src[c] * w;
            }
            weight[out_pos + n] += w;
        }
        previous = start;
    }
    for (std::size_t i = 0; i < out_frames; ++i) {
        if (weight[i] > 1e-3f) {
            for (std::size_t c = 0; c < channels; ++c) {
                out[i * channels + c] /= weight[i];
            }
        }
    }
    out.resize(out_frames * channels);
    return out;
}

std::vector<float> pitch_shift(const float* interleaved, std::size_t frames, std::size_t channels,
                               double semitones) {
    if (interleaved == nullptr || frames == 0 || channels == 0) {
        return {};
    }
    const double ratio = std::pow(2.0, std::clamp(semitones, -24.0, 24.0) / 12.0);
    // Longer by the ratio at the same pitch, then played that much faster: the same length at
    // a pitch moved by the ratio.
    const std::vector<float> stretched = time_stretch(interleaved, frames, channels, ratio);
    const std::size_t stretched_frames = stretched.size() / channels;
    std::vector<float> out(frames * channels, 0.0f);
    if (stretched_frames < 2) {
        return out;
    }
    for (std::size_t i = 0; i < frames; ++i) {
        const double position = static_cast<double>(i) * ratio;
        const auto index = static_cast<std::size_t>(position);
        if (index + 1 >= stretched_frames) {
            break;
        }
        const auto frac = static_cast<float>(position - static_cast<double>(index));
        for (std::size_t c = 0; c < channels; ++c) {
            const float a = stretched[index * channels + c];
            const float b = stretched[(index + 1) * channels + c];
            out[i * channels + c] = a + (b - a) * frac;
        }
    }
    return out;
}

}  // namespace toh::core
