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
        case Instrument::FmKeys:
            return {0.003, 1.5, 0.3, 0.25};
        case Instrument::FmBell:
            return {0.002, 2.5, 0.0, 1.5};
        case Instrument::AnalogBass:
            return {0.003, 0.4, 0.7, 0.08};
        case Instrument::Supersaw:
            return {0.01, 0.0, 1.0, 0.3};
        case Instrument::WavetableLead:
            return {0.01, 0.3, 0.85, 0.15};
        case Instrument::SynthBrass:
            return {0.06, 0.5, 0.8, 0.2};
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

// A sum of harmonics n = 1, 2, ... of the phase `w` below `nyquist`, each with the amplitude
// `amplitude(n)`. The sines come from a recurrence instead of one std::sin per harmonic.
template <typename Amplitude>
double harmonics(double w, double frequency, double nyquist, int most, Amplitude amplitude) noexcept {
    double previous = 0.0;
    double current = std::sin(w);
    const double twice_cos = 2.0 * std::cos(w);
    double sum = 0.0;
    for (int n = 1; n <= most; ++n) {
        if (frequency * static_cast<double>(n) >= nyquist) {
            break;
        }
        sum += amplitude(n) * current;
        const double next = twice_cos * current - previous;
        previous = current;
        current = next;
    }
    return sum;
}

// A low-pass filter as a spectrum: how much of a harmonic at `f` passes a cutoff `fc`
// (24 dB per octave above it).
double low_pass(double f, double fc) noexcept {
    const double x = f / fc;
    return 1.0 / std::sqrt(1.0 + x * x * x * x);
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
        case Instrument::FmKeys: {
            // The modulator at the same frequency: bright when struck, mellow when held.
            const double index = (0.6 + 1.6 * static_cast<double>(note.velocity)) * std::exp(-t * 3.0) + 0.25;
            value = std::sin(w + index * std::sin(w));
            if (frequency * 14.0 < nyquist) {
                value += 0.2 * std::sin(14.0 * w) * std::exp(-t * 25.0);  // the tine
            }
            value *= 0.8;
            break;
        }
        case Instrument::FmBell: {
            // A modulator at 3.5 times the frequency gives the inharmonic partials of a bell.
            double index = 2.5 * std::exp(-t * 1.2) + 0.2;
            if (frequency * 3.5 * (index + 2.0) > nyquist) {
                index = std::max(0.0, nyquist / (frequency * 3.5) - 2.0);
            }
            value = std::sin(w + index * std::sin(3.5 * w)) * std::exp(-t * 0.6);
            break;
        }
        case Instrument::AnalogBass: {
            const double cutoff = frequency * (1.5 + 10.0 * std::exp(-t * 12.0));
            value = harmonics(w, frequency, nyquist, 40, [&](int n) {
                const double f = frequency * static_cast<double>(n);
                // A little resonance where the filter cuts.
                const double peak = (f - cutoff) / (0.25 * cutoff);
                return (low_pass(f, cutoff) + 0.6 * std::exp(-peak * peak)) / static_cast<double>(n);
            });
            value = 0.55 * value + 0.45 * std::sin(0.5 * w);  // the sub octave
            break;
        }
        case Instrument::Supersaw: {
            static const double detune[5] = {-0.011, -0.0055, 0.0, 0.0055, 0.011};
            static const double phase[5] = {0.0, 1.7, 3.1, 4.4, 5.6};
            for (int v = 0; v < 5; ++v) {
                const double f = frequency * (1.0 + detune[v]);
                value += harmonics(kTwoPi * f * t + phase[v], f, nyquist, 16, [](int n) {
                    const double x = static_cast<double>(n) / 12.0;
                    return 1.0 / (static_cast<double>(n) * std::sqrt(1.0 + x * x));
                });
            }
            value *= 0.32;
            break;
        }
        case Instrument::WavetableLead: {
            const double vibrato = 0.3 * std::sin(kTwoPi * 5.5 * t) * std::min(t / 0.5, 1.0);
            const double morph = 0.5 - 0.5 * std::cos(kTwoPi * 0.25 * t);  // 0 = saw, 1 = square
            const double bright = 4.0 + 12.0 * morph;
            value = harmonics(w + vibrato, frequency, nyquist, 24, [&](int n) {
                const double x = static_cast<double>(n) / bright;
                const double even = (n % 2 == 0) ? 1.0 - morph : 1.0;
                return even / (static_cast<double>(n) * (1.0 + x * x));
            });
            value *= 0.9;
            break;
        }
        case Instrument::SynthBrass: {
            const double swell = std::min(t / 0.08, 1.0) * (0.6 + 0.4 * std::exp(-t * 2.0));
            const double cutoff = frequency * (1.2 + 7.0 * swell * (0.5 + 0.5 * static_cast<double>(note.velocity)));
            value = harmonics(w, frequency, nyquist, 30, [&](int n) {
                return low_pass(frequency * static_cast<double>(n), cutoff) / static_cast<double>(n);
            });
            value *= 0.8;
            break;
        }
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
