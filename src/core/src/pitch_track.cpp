#include "toh/core/pitch_track.h"

#include <algorithm>
#include <cmath>

namespace toh::core {

PitchCurve track_pitch(const float* interleaved, std::size_t frames, std::size_t channels, double sample_rate) {
    PitchCurve curve;
    if (interleaved == nullptr || channels == 0 || sample_rate <= 0.0 || frames == 0) {
        return curve;
    }
    // A mono copy at a quarter of the rate (a short low-pass filter first: the average of 8).
    constexpr std::size_t kDown = 4;
    const double rate = sample_rate / static_cast<double>(kDown);
    std::vector<float> mono(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (std::size_t c = 0; c < channels; ++c) {
            sum += interleaved[i * channels + c];
        }
        mono[i] = sum / static_cast<float>(channels);
    }
    std::vector<float> low(frames / kDown);
    for (std::size_t i = 0; i < low.size(); ++i) {
        float sum = 0.0f;
        int count = 0;
        for (std::size_t k = i * kDown; k < i * kDown + 8 && k < frames; ++k) {
            sum += mono[k];
            ++count;
        }
        low[i] = sum / static_cast<float>(std::max(count, 1));
    }

    const auto shortest = static_cast<std::size_t>(rate / 1000.0);  // the period at 1 kHz
    const auto longest = static_cast<std::size_t>(rate / 60.0);     // and at 60 Hz
    const std::size_t window = longest + 16;
    curve.hop = 512;
    const std::size_t step = curve.hop / kDown;
    const std::size_t hops = frames / curve.hop;
    curve.midi.assign(hops, -1.0f);

    // How loud the loudest part is, to leave out what is much quieter.
    float loudest = 0.0f;
    for (const float value : low) {
        loudest = std::max(loudest, std::fabs(value));
    }
    if (loudest <= 1e-6f) {
        return curve;
    }
    const double quiet = static_cast<double>(loudest) * 0.03;  // about -30 dB

    std::vector<double> difference(longest + 2, 0.0);
    for (std::size_t h = 0; h < hops; ++h) {
        const std::size_t begin = h * step;
        if (begin + window + longest + 1 >= low.size()) {
            break;
        }
        double energy = 0.0;
        for (std::size_t j = 0; j < window; ++j) {
            energy += static_cast<double>(low[begin + j]) * low[begin + j];
        }
        if (std::sqrt(energy / static_cast<double>(window)) < quiet) {
            continue;
        }
        // The YIN difference, normalised by its running mean.
        double running = 0.0;
        std::size_t found = 0;
        for (std::size_t tau = 1; tau <= longest + 1; ++tau) {
            double sum = 0.0;
            for (std::size_t j = 0; j < window; ++j) {
                const double d = static_cast<double>(low[begin + j]) - low[begin + j + tau];
                sum += d * d;
            }
            running += sum;
            difference[tau] = running > 0.0 ? sum * static_cast<double>(tau) / running : 1.0;
        }
        for (std::size_t tau = shortest; tau <= longest; ++tau) {
            if (difference[tau] < 0.15) {
                while (tau + 1 <= longest && difference[tau + 1] < difference[tau]) {
                    ++tau;
                }
                found = tau;
                break;
            }
        }
        if (found == 0) {
            continue;
        }
        // A finer period from the parabola through the neighbours.
        double period = static_cast<double>(found);
        const double a = difference[found - 1];
        const double b = difference[found];
        const double c = difference[found + 1];
        if (a - 2.0 * b + c > 0.0) {
            period += 0.5 * (a - c) / (a - 2.0 * b + c);
        }
        const double frequency = rate / period;
        curve.midi[h] = static_cast<float>(69.0 + 12.0 * std::log2(frequency / 440.0));
    }
    return curve;
}

std::vector<PitchNote> find_pitch_notes(const PitchCurve& curve) {
    std::vector<PitchNote> notes;
    const std::size_t hops = curve.midi.size();
    const auto hop = static_cast<std::int64_t>(curve.hop);
    constexpr std::size_t kShortest = 5;  // hops (about 50 ms at 48 kHz)
    std::size_t start = 0;
    std::vector<float> values;
    const auto finish = [&](std::size_t end) {
        if (end > start && end - start >= kShortest && !values.empty()) {
            std::vector<float> sorted = values;
            std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
            PitchNote note;
            note.start = static_cast<std::int64_t>(start) * hop;
            note.length = static_cast<std::int64_t>(end - start) * hop;
            note.pitch = sorted[sorted.size() / 2];
            notes.push_back(note);
        }
        values.clear();
    };
    bool inside = false;
    std::size_t gap = 0;
    for (std::size_t h = 0; h < hops; ++h) {
        const float pitch = curve.midi[h];
        if (pitch < 0.0f) {
            if (inside && ++gap >= 3) {
                finish(h - gap + 1);
                inside = false;
            }
            continue;
        }
        gap = 0;
        if (!inside) {
            inside = true;
            start = h;
            values.assign(1, pitch);
            continue;
        }
        // A jump that holds for three hops starts a new note.
        float centre = values.back();
        if (values.size() >= 3) {
            std::vector<float> recent(values.end() - 3, values.end());
            std::sort(recent.begin(), recent.end());
            centre = recent[1];
        }
        if (std::fabs(pitch - centre) > 0.6f) {
            bool holds = true;
            for (std::size_t k = h + 1; k < std::min(h + 3, hops); ++k) {
                if (curve.midi[k] < 0.0f || std::fabs(curve.midi[k] - pitch) > 0.6f) {
                    holds = false;
                }
            }
            if (holds) {
                finish(h);
                start = h;
                values.assign(1, pitch);
                continue;
            }
        }
        values.push_back(pitch);
    }
    if (inside) {
        finish(hops);
    }
    return notes;
}

}  // namespace toh::core
