#include <cmath>

#include "toh/core/notes.h"

namespace toh::core {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

// The loudness over time: attack, decay towards the sustain level, and a straight release.
struct Shape {
    double attack;   // seconds
    double decay;    // seconds (time constant); 0 for none
    double sustain;  // level after the decay
    double release;  // seconds
};

Shape shape_of(Instrument instrument) noexcept {
    switch (instrument) {
        case Instrument::Organ:
            return {0.01, 0.0, 1.0, 0.06};
        case Instrument::Pad:
            return {0.35, 0.6, 0.8, 0.4};
        case Instrument::Pluck:
            return {0.003, 0.0, 1.0, 0.08};
        case Instrument::Keys:
            break;
    }
    return {0.005, 1.2, 0.35, 0.12};
}

double level_while_held(const Shape& shape, double t) noexcept {
    if (t < shape.attack) {
        return t / shape.attack;
    }
    if (shape.decay <= 0.0) {
        return 1.0;
    }
    return shape.sustain + (1.0 - shape.sustain) * std::exp(-(t - shape.attack) / shape.decay);
}

double envelope(const Shape& shape, double t, double held) noexcept {
    if (t < held) {
        return level_while_held(shape, t);
    }
    const double released = t - held;
    if (released >= shape.release) {
        return 0.0;
    }
    return level_while_held(shape, held) * (1.0 - released / shape.release);
}

}  // namespace

double instrument_release_seconds(Instrument instrument) noexcept {
    return shape_of(instrument).release;
}

float note_sample(Instrument instrument, const Note& note, double t_frames,
                  double sample_rate) noexcept {
    if (t_frames < 0.0 || sample_rate <= 0.0) {
        return 0.0f;
    }
    const double t = t_frames / sample_rate;
    const double held = static_cast<double>(note.length) / sample_rate;
    const double level = envelope(shape_of(instrument), t, held);
    if (level <= 0.0) {
        return 0.0f;
    }
    const double frequency = 440.0 * std::pow(2.0, (static_cast<double>(note.pitch) - 69.0) / 12.0);
    const double nyquist = sample_rate * 0.5;
    const double w = kTwoPi * frequency * t;
    double value = 0.0;
    switch (instrument) {
        case Instrument::Organ:
            for (const double k : {1.0, 2.0, 4.0}) {
                if (frequency * k < nyquist) {
                    value += std::sin(k * w) / k;
                }
            }
            value *= 0.6;
            break;
        case Instrument::Pad:
            // Two slightly detuned sets of harmonics, falling off like a soft saw wave.
            for (int k = 1; k <= 6; ++k) {
                const double n = static_cast<double>(k);
                if (frequency * n * 1.003 < nyquist) {
                    value += (std::sin(n * w * 1.003) + std::sin(n * w * 0.997)) / (2.0 * n);
                }
            }
            value *= 0.7;
            break;
        case Instrument::Pluck:
            // Higher harmonics die away faster, like on a string.
            for (int k = 1; k <= 5; ++k) {
                const double n = static_cast<double>(k);
                if (frequency * n < nyquist) {
                    value += std::exp(-t * (1.5 + 1.5 * n)) * std::sin(n * w) / n;
                }
            }
            break;
        case Instrument::Keys:
            value = std::sin(w);
            if (frequency * 2.0 < nyquist) {
                value += 0.4 * std::sin(2.0 * w) * std::exp(-t * 2.0);
            }
            if (frequency * 3.0 < nyquist) {
                value += 0.15 * std::sin(3.0 * w) * std::exp(-t * 4.0);
            }
            value *= 0.8;
            break;
    }
    return static_cast<float>(value * level * static_cast<double>(note.velocity) * 0.3);
}

}  // namespace toh::core
