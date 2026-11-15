#include "toh/core/stock_sounds.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace toh::core {
namespace {

constexpr double kPi = 3.14159265358979323846;

// A buffer of two sides that the generators add to.
struct Buf {
    double sr = 48000.0;
    std::vector<double> left;
    std::vector<double> right;
    std::size_t size() const noexcept { return left.size(); }
    double t(std::size_t i) const noexcept { return static_cast<double>(i) / sr; }
    double seconds() const noexcept { return static_cast<double>(size()) / sr; }
    void add(std::size_t i, double value) noexcept {
        left[i] += value;
        right[i] += value;
    }
};

// White noise that is the same every time (xorshift).
struct Noise {
    std::uint32_t state;
    explicit Noise(std::uint32_t seed) : state(seed != 0 ? seed : 1u) {}
    double next() noexcept {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return static_cast<double>(state) / 2147483647.5 - 1.0;
    }
};

// A state-variable filter (topology-preserving); one call gives all three outputs.
struct Svf {
    double ic1 = 0.0;
    double ic2 = 0.0;
    double low = 0.0;
    double band = 0.0;
    double high = 0.0;
    void run(double x, double cutoff, double q, double sr) noexcept {
        const double g = std::tan(kPi * std::clamp(cutoff, 10.0, 0.45 * sr) / sr);
        const double k = 1.0 / std::max(q, 0.05);
        const double a1 = 1.0 / (1.0 + g * (g + k));
        const double a2 = g * a1;
        const double a3 = g * a2;
        const double v3 = x - ic2;
        const double v1 = a1 * ic1 + a2 * v3;
        const double v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0 * v1 - ic1;
        ic2 = 2.0 * v2 - ic2;
        low = v2;
        band = v1;
        high = x - k * v1 - v2;
    }
};

double fraction(double phase) noexcept { return phase - std::floor(phase); }
double saw(double phase) noexcept { return 2.0 * fraction(phase) - 1.0; }
double square(double phase) noexcept { return fraction(phase) < 0.5 ? 1.0 : -1.0; }

// Six detuned square waves: the metal of hi-hats and cymbals, as in classic drum machines.
double metal(double t) noexcept {
    static const double kFrequencies[6] = {263.0, 400.0, 421.0, 474.0, 587.0, 845.0};
    double sum = 0.0;
    for (const double f : kFrequencies) {
        sum += square(t * f * 1.7);
    }
    return sum / 6.0;
}

// A drum whose pitch falls from `high` to `low` hertz: kicks and toms.
void falling_drum(Buf& b, double low, double high, double drop, double decay, double drive) {
    double phase = 0.0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        phase += (low + (high - low) * std::exp(-t * drop)) / b.sr;
        double v = std::sin(2.0 * kPi * phase) * std::exp(-t * decay);
        if (drive > 0.0) {
            v = std::tanh(drive * v) / std::tanh(drive);
        }
        b.add(i, v);
    }
}

void kick(Buf& b) {
    falling_drum(b, 50.0, 160.0, 35.0, 7.0, 1.5);
    Noise noise(11);
    Svf filter;
    for (std::size_t i = 0; i < b.size(); ++i) {
        filter.run(noise.next(), 3000.0, 0.7, b.sr);
        b.add(i, 0.25 * filter.high * std::exp(-b.t(i) * 400.0));
    }
}
void kick_808(Buf& b) { falling_drum(b, 48.0, 110.0, 20.0, 2.2, 1.8); }
void kick_punchy(Buf& b) { falling_drum(b, 60.0, 260.0, 50.0, 10.0, 3.0); }

void snare(Buf& b) {
    Noise noise(23);
    Svf filter;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        const double tone = (std::sin(2.0 * kPi * 185.0 * t) + 0.6 * std::sin(2.0 * kPi * 330.0 * t)) *
                            std::exp(-t * 18.0) * 0.5;
        filter.run(noise.next(), 3000.0, 0.7, b.sr);
        b.add(i, tone + filter.band * 1.6 * std::exp(-t * 14.0));
    }
}

void clap(Buf& b) {
    Noise noise(37);
    Svf filter;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        double envelope = 0.0;
        for (const double at : {0.0, 0.011, 0.022}) {
            if (t >= at) {
                envelope = std::max(envelope, std::exp(-(t - at) * 300.0));
            }
        }
        if (t >= 0.031) {
            envelope = std::max(envelope, std::exp(-(t - 0.031) * 12.0));
        }
        filter.run(noise.next(), 1500.0, 1.5, b.sr);
        b.add(i, filter.band * 2.0 * envelope);
    }
}

void rimshot(Buf& b) {
    Noise noise(41);
    Svf filter;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        filter.run(noise.next(), 4000.0, 0.7, b.sr);
        b.add(i, std::sin(2.0 * kPi * 1700.0 * t) * std::exp(-t * 60.0) + 0.5 * filter.high * std::exp(-t * 80.0));
    }
}

void hat(Buf& b, double decay, std::uint32_t seed) {
    Noise noise(seed);
    Svf filter;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        filter.run(metal(t) * 0.7 + noise.next() * 0.3, 7500.0, 0.8, b.sr);
        b.add(i, filter.high * std::exp(-t * decay));
    }
}
void hat_closed(Buf& b) { hat(b, 60.0, 53); }
void hat_open(Buf& b) { hat(b, 6.0, 59); }

void cymbal(Buf& b, double decay, double bell, std::uint32_t seed) {
    Noise noise(seed);
    Svf filter;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        filter.run(metal(t * 1.3) * 0.5 + noise.next() * 0.5, 5000.0, 0.6, b.sr);
        const double ring = bell * std::sin(2.0 * kPi * 3200.0 * t) * std::exp(-t * 3.0);
        const double attack = std::min(1.0, t / 0.002);
        b.add(i, (filter.high + ring) * std::exp(-t * decay) * attack);
    }
}
void crash(Buf& b) { cymbal(b, 1.8, 0.0, 61); }
void ride(Buf& b) { cymbal(b, 1.2, 0.35, 67); }

void tom_low(Buf& b) { falling_drum(b, 90.0, 130.0, 15.0, 6.0, 1.2); }
void tom_mid(Buf& b) { falling_drum(b, 130.0, 180.0, 15.0, 7.0, 1.2); }
void tom_high(Buf& b) { falling_drum(b, 180.0, 240.0, 15.0, 8.0, 1.2); }

void cowbell(Buf& b) {
    Svf filter;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        filter.run(square(540.0 * t) + square(800.0 * t), 800.0, 2.0, b.sr);
        b.add(i, filter.band * std::exp(-t * 12.0));
    }
}

void shaker(Buf& b) {
    Noise noise(71);
    Svf filter;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        filter.run(noise.next(), 6000.0, 1.0, b.sr);
        const double envelope = t < 0.02 ? t / 0.02 : std::exp(-(t - 0.02) * 25.0);
        b.add(i, filter.band * envelope);
    }
}

void tambourine(Buf& b) {
    Noise noise(73);
    Svf filter;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        filter.run(noise.next(), 6000.0, 0.7, b.sr);
        const double jingles = std::sin(2.0 * kPi * 5200.0 * t) + std::sin(2.0 * kPi * 6800.0 * t) +
                               std::sin(2.0 * kPi * 7900.0 * t);
        b.add(i, (0.6 * filter.high + 0.25 * jingles) * std::exp(-t * 9.0));
    }
}

void woodblock(Buf& b) {
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        b.add(i, (std::sin(2.0 * kPi * 900.0 * t) + 0.3 * std::sin(2.0 * kPi * 1500.0 * t)) * std::exp(-t * 45.0));
    }
}
void conga(Buf& b) { falling_drum(b, 200.0, 230.0, 30.0, 12.0, 0.0); }

void riser(Buf& b) {
    Noise noise(79);
    Svf filter;
    double phase = 0.0;
    const double length = b.seconds();
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double x = b.t(i) / length;  // 0 to 1
        filter.run(noise.next(), 300.0 * std::pow(30.0, x), 2.0, b.sr);
        phase += (100.0 * std::pow(12.0, x)) / b.sr;
        b.add(i, (filter.band * 1.5 + 0.2 * std::sin(2.0 * kPi * phase)) * x * x);
    }
}

void downlifter(Buf& b) {
    Noise noise(83);
    Svf filter;
    const double length = b.seconds();
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double x = b.t(i) / length;
        filter.run(noise.next(), 8000.0 * std::pow(1.0 / 40.0, x), 2.0, b.sr);
        b.add(i, filter.band * 1.5 * (1.0 - x));
    }
}

void impact(Buf& b) {
    Noise noise(89);
    Svf filter;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        filter.run(noise.next(), 200.0 + 4000.0 * std::exp(-t * 6.0), 0.7, b.sr);
        b.add(i, std::sin(2.0 * kPi * 45.0 * t) * std::exp(-t * 2.0) + 0.6 * filter.low * std::exp(-t * 4.0));
    }
}

void sub_drop(Buf& b) { falling_drum(b, 30.0, 80.0, 1.5, 1.5, 1.2); }

void zap(Buf& b) {
    double phase = 0.0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        phase += (100.0 + 1900.0 * std::exp(-t * 15.0)) / b.sr;
        b.add(i, square(phase) * 0.5 * std::exp(-t * 8.0));
    }
}

void noise_sweep(Buf& b) {
    Noise noise(97);
    Svf filter;
    const double length = b.seconds();
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double x = b.t(i) / length;
        filter.run(noise.next(), 100.0 * std::pow(80.0, x), 0.7, b.sr);
        b.add(i, filter.high * std::sin(kPi * x));
    }
}

// Saw waves through a low-pass filter whose cutoff closes: plucks, stabs, pads.
void saw_voice(Buf& b, const std::vector<double>& frequencies, double cutoff, double closing,
               double attack, double decay, double detune) {
    Svf filter_l;
    Svf filter_r;
    const double length = b.seconds();
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        double l = 0.0;
        double r = 0.0;
        for (const double f : frequencies) {
            l += saw(f * (1.0 - detune) * t);
            r += saw(f * (1.0 + detune) * t);
        }
        const double c = 100.0 + cutoff * std::exp(-t * closing);
        filter_l.run(l / static_cast<double>(frequencies.size()), c, 0.9, b.sr);
        filter_r.run(r / static_cast<double>(frequencies.size()), c, 0.9, b.sr);
        double envelope = (attack > 0.0 ? std::min(1.0, t / attack) : 1.0) * std::exp(-t * decay);
        envelope *= std::min(1.0, (length - t) / 0.02);  // no click at the end
        b.left[i] += filter_l.low * envelope;
        b.right[i] += filter_r.low * envelope;
    }
}
void bass_pluck(Buf& b) { saw_voice(b, {65.41}, 2000.0, 12.0, 0.002, 3.0, 0.0); }
void synth_stab(Buf& b) { saw_voice(b, {261.63, 311.13, 392.0}, 3000.0, 4.0, 0.003, 6.0, 0.003); }
void pad(Buf& b) {
    saw_voice(b, {130.81, 261.63, 329.63, 392.0}, 1200.0, 0.0, 0.8, 0.25, 0.006);
}

void bell(Buf& b) {
    for (std::size_t i = 0; i < b.size(); ++i) {
        const double t = b.t(i);
        const double index = 4.0 * std::exp(-t * 3.0);
        const double modulator = std::sin(2.0 * kPi * 523.25 * 3.5 * t) * index;
        b.add(i, std::sin(2.0 * kPi * 523.25 * t + modulator) * std::exp(-t * 2.0));
    }
}

// A plucked string (Karplus-Strong) at A4.
void pluck(Buf& b) {
    Noise noise(101);
    const auto period = static_cast<std::size_t>(std::lround(b.sr / 440.0));
    std::vector<double> line(std::max<std::size_t>(period, 2));
    for (double& value : line) {
        value = noise.next();
    }
    std::size_t at = 0;
    for (std::size_t i = 0; i < b.size(); ++i) {
        const std::size_t next = (at + 1) % line.size();
        const double value = line[at];
        line[at] = 0.996 * 0.5 * (line[at] + line[next]);
        at = next;
        b.add(i, value);
    }
}

struct Entry {
    StockSoundInfo info;
    double seconds;
    void (*make)(Buf&);
};

const Entry kSounds[] = {
    {{"Drums", "Kick"}, 0.6, kick},
    {{"Drums", "Kick 808"}, 1.6, kick_808},
    {{"Drums", "Punchy kick"}, 0.4, kick_punchy},
    {{"Drums", "Snare"}, 0.35, snare},
    {{"Drums", "Clap"}, 0.5, clap},
    {{"Drums", "Rimshot"}, 0.12, rimshot},
    {{"Drums", "Closed hi-hat"}, 0.12, hat_closed},
    {{"Drums", "Open hi-hat"}, 0.6, hat_open},
    {{"Drums", "Crash"}, 2.5, crash},
    {{"Drums", "Ride"}, 2.0, ride},
    {{"Drums", "Low tom"}, 0.7, tom_low},
    {{"Drums", "Mid tom"}, 0.6, tom_mid},
    {{"Drums", "High tom"}, 0.5, tom_high},
    {{"Percussion", "Cowbell"}, 0.4, cowbell},
    {{"Percussion", "Shaker"}, 0.2, shaker},
    {{"Percussion", "Tambourine"}, 0.4, tambourine},
    {{"Percussion", "Woodblock"}, 0.15, woodblock},
    {{"Percussion", "Conga"}, 0.4, conga},
    {{"Effects", "Riser"}, 4.0, riser},
    {{"Effects", "Downlifter"}, 3.0, downlifter},
    {{"Effects", "Impact"}, 2.0, impact},
    {{"Effects", "Sub drop"}, 2.5, sub_drop},
    {{"Effects", "Zap"}, 0.4, zap},
    {{"Effects", "Noise sweep"}, 2.0, noise_sweep},
    {{"Tones", "Bass pluck (C)"}, 1.0, bass_pluck},
    {{"Tones", "Synth stab (C minor)"}, 0.6, synth_stab},
    {{"Tones", "Pad (C major)"}, 4.0, pad},
    {{"Tones", "Bell (C)"}, 3.0, bell},
    {{"Tones", "Plucked string (A)"}, 1.5, pluck},
};

}  // namespace

std::size_t stock_sound_count() noexcept { return sizeof(kSounds) / sizeof(kSounds[0]); }

StockSoundInfo stock_sound_info(std::size_t index) noexcept {
    return index < stock_sound_count() ? kSounds[index].info : StockSoundInfo{"", ""};
}

std::shared_ptr<const ClipData> make_stock_sound(std::size_t index, std::uint32_t sample_rate,
                                                 std::uint32_t channels) {
    if (index >= stock_sound_count() || sample_rate == 0 || channels == 0) {
        return nullptr;
    }
    const Entry& entry = kSounds[index];
    Buf b;
    b.sr = static_cast<double>(sample_rate);
    const auto frames = static_cast<std::size_t>(std::lround(entry.seconds * b.sr));
    b.left.assign(frames, 0.0);
    b.right.assign(frames, 0.0);
    entry.make(b);
    // A short fade at the end, and the peak at -2 dB.
    double peak = 0.0;
    const std::size_t fade = std::min<std::size_t>(frames, static_cast<std::size_t>(b.sr * 0.005));
    for (std::size_t i = 0; i < frames; ++i) {
        if (i + fade >= frames && fade > 0) {
            const double gain = static_cast<double>(frames - i) / static_cast<double>(fade);
            b.left[i] *= gain;
            b.right[i] *= gain;
        }
        peak = std::max(peak, std::max(std::fabs(b.left[i]), std::fabs(b.right[i])));
    }
    const double scale = peak > 0.0 ? 0.79 / peak : 0.0;
    auto clip = std::make_shared<ClipData>();
    clip->channels = channels;
    clip->sample_rate = sample_rate;
    clip->samples.resize(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        const auto l = static_cast<float>(b.left[i] * scale);
        const auto r = static_cast<float>(b.right[i] * scale);
        float* frame = clip->samples.data() + i * channels;
        if (channels == 1) {
            frame[0] = 0.5f * (l + r);
            continue;
        }
        frame[0] = l;
        frame[1] = r;
        for (std::size_t c = 2; c < channels; ++c) {
            frame[c] = 0.5f * (l + r);
        }
    }
    return clip;
}

}  // namespace toh::core
