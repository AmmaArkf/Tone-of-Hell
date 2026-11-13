#include "toh/core/sampler.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <utility>

namespace toh::core {

namespace {

constexpr double kPi = 3.14159265358979323846;

// An in-place radix-2 FFT; the size must be a power of two.
void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i], a[j]);
        }
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<double> u = a[i + k];
                const std::complex<double> v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= step;
            }
        }
    }
}

// Where a peak really is, between bins: a parabola through the log magnitudes around it.
double refine(const std::vector<double>& mag, std::size_t k) {
    if (k == 0 || k + 1 >= mag.size()) {
        return static_cast<double>(k);
    }
    const double a = std::log(mag[k - 1] + 1e-12);
    const double b = std::log(mag[k] + 1e-12);
    const double c = std::log(mag[k + 1] + 1e-12);
    const double denominator = a - 2.0 * b + c;
    if (std::fabs(denominator) < 1e-12) {
        return static_cast<double>(k);
    }
    return static_cast<double>(k) + std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5);
}

}  // namespace

PitchAnalysis analyze_pitch(const float* mono, std::size_t frames, double sample_rate) {
    PitchAnalysis result;
    result.spectrum.assign(kSpectrumBins, 0.0f);
    if (mono == nullptr || frames == 0 || !(sample_rate > 0.0)) {
        return result;
    }
    std::size_t n = 2048;
    while (n * 2 <= std::min<std::size_t>(frames, 16384)) {
        n *= 2;
    }

    // The loudest stretch of n frames.
    std::size_t best_start = 0;
    double best_energy = -1.0;
    for (std::size_t start = 0;; start += n / 4) {
        const std::size_t stop = std::min(frames, start + n);
        double energy = 0.0;
        for (std::size_t i = start; i < stop; ++i) {
            energy += static_cast<double>(mono[i]) * static_cast<double>(mono[i]);
        }
        if (energy > best_energy) {
            best_energy = energy;
            best_start = start;
        }
        if (start + n >= frames) {
            break;
        }
    }
    if (best_energy < 1e-9 * static_cast<double>(n)) {
        return result;  // silence
    }

    std::vector<std::complex<double>> buffer(n);
    for (std::size_t i = 0; i < n; ++i) {
        const std::size_t at = best_start + i;
        const double value = at < frames ? static_cast<double>(mono[at]) : 0.0;
        const double window =
            0.5 - 0.5 * std::cos(2.0 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        buffer[i] = std::complex<double>(value * window, 0.0);
    }
    fft(buffer);
    std::vector<double> mag(n / 2);
    for (std::size_t k = 0; k < mag.size(); ++k) {
        mag[k] = std::abs(buffer[k]);
    }
    const double bin_hz = sample_rate / static_cast<double>(n);

    // The spectrum for drawing.
    double top = 1e-12;
    for (std::size_t k = 1; k < mag.size(); ++k) {
        const double hz = static_cast<double>(k) * bin_hz;
        if (hz >= kSpectrumLow && hz <= kSpectrumHigh) {
            top = std::max(top, mag[k]);
        }
    }
    for (std::size_t b = 0; b < kSpectrumBins; ++b) {
        const double ratio = kSpectrumHigh / kSpectrumLow;
        const double bins = static_cast<double>(kSpectrumBins);
        const double f0 = kSpectrumLow * std::pow(ratio, static_cast<double>(b) / bins);
        const double f1 = kSpectrumLow * std::pow(ratio, static_cast<double>(b + 1) / bins);
        auto k0 = static_cast<std::size_t>(std::floor(f0 / bin_hz));
        auto k1 = static_cast<std::size_t>(std::ceil(f1 / bin_hz));
        k0 = std::min(k0, mag.size() - 1);
        k1 = std::clamp(k1, k0, mag.size() - 1);
        double level = 0.0;
        for (std::size_t k = k0; k <= k1; ++k) {
            level = std::max(level, mag[k]);
        }
        const double db = 20.0 * std::log10(level / top + 1e-12);
        result.spectrum[b] = static_cast<float>(std::clamp((db + 60.0) / 60.0, 0.0, 1.0));
    }

    // The loudest peak between 25 Hz and 5 kHz.
    const auto low = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(25.0 / bin_hz)));
    const auto high = std::min<std::size_t>(mag.size() - 2, static_cast<std::size_t>(5000.0 / bin_hz));
    if (low >= high) {
        return result;
    }
    std::size_t peak = low;
    for (std::size_t k = low; k <= high; ++k) {
        if (mag[k] > mag[peak]) {
            peak = k;
        }
    }
    // The loudest peak may be an overtone: a clear peak at a half, a third ... of its frequency
    // is the fundamental that the ear hears as the note.
    const double peak_at = refine(mag, peak);
    std::size_t fundamental = peak;
    for (int divisor = 2; divisor <= 6; ++divisor) {
        const double target = peak_at / divisor;
        if (target < static_cast<double>(low)) {
            break;
        }
        const auto centre = static_cast<std::size_t>(std::llround(target));
        std::size_t best = centre;
        for (std::size_t k = centre > 2 ? centre - 2 : 1; k <= centre + 2 && k + 1 < mag.size(); ++k) {
            if (mag[k] > mag[best]) {
                best = k;
            }
        }
        const bool is_peak = mag[best] >= mag[best - 1] && mag[best] >= mag[best + 1];
        if (is_peak && mag[best] > 0.2 * mag[peak]) {
            fundamental = best;
        }
    }
    result.frequency = refine(mag, fundamental) * bin_hz;
    if (result.frequency > 0.0) {
        result.note = std::clamp(69.0 + 12.0 * std::log2(result.frequency / 440.0), 0.0, 127.0);
    }
    return result;
}

std::shared_ptr<const SamplerSound> make_sampler_sound(std::string name, std::vector<float> mono,
                                                       double sample_rate, double root) {
    auto sound = std::make_shared<SamplerSound>();
    PitchAnalysis analysis = analyze_pitch(mono.data(), mono.size(), sample_rate);
    sound->name = std::move(name);
    sound->sample_rate = sample_rate;
    sound->detected = analysis.note;
    sound->frequency = analysis.frequency;
    sound->spectrum = std::move(analysis.spectrum);
    sound->root = root >= 0.0 ? root : (analysis.note >= 0.0 ? analysis.note : 60.0);
    sound->data = std::make_shared<const std::vector<float>>(std::move(mono));
    return sound;
}

float sampler_note(const SamplerSound& sound, const Note& note, double t_frames,
                   double sample_rate) noexcept {
    if (t_frames < 0.0 || !sound.data || sound.data->size() < 2) {
        return 0.0f;
    }
    const double end = static_cast<double>(note.length);
    const double release = kSamplerReleaseSeconds * sample_rate;
    if (t_frames >= end + release) {
        return 0.0f;
    }
    const double step = std::exp2((static_cast<double>(note.pitch) - sound.root) / 12.0) *
                        sound.sample_rate / sample_rate;
    const double position = t_frames * step;
    const std::vector<float>& data = *sound.data;
    const auto index = static_cast<std::size_t>(position);
    if (index + 1 >= data.size()) {
        return 0.0f;  // the sound is over
    }
    const auto frac = static_cast<float>(position - static_cast<double>(index));
    const float value = data[index] + (data[index + 1] - data[index]) * frac;
    double envelope = std::min(t_frames / (0.002 * sample_rate), 1.0);  // no click at the start
    if (t_frames > end) {
        envelope *= 1.0 - (t_frames - end) / release;
    }
    return value * static_cast<float>(envelope) * note.velocity;
}

}  // namespace toh::core
