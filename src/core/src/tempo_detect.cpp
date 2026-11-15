#include "toh/core/tempo_detect.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace toh::core {
namespace {

constexpr double kTwoPi = 6.283185307179586476925286766559;

}  // namespace

TempoEstimate detect_tempo(const float* interleaved, std::size_t frames, std::size_t channels,
                           double sample_rate) {
    TempoEstimate result;
    if (interleaved == nullptr || channels == 0 || sample_rate <= 0.0) {
        return result;
    }
    frames = std::min(frames, static_cast<std::size_t>(120.0 * sample_rate));
    const std::size_t hop = std::max<std::size_t>(static_cast<std::size_t>(sample_rate / 187.5), 1);  // 256 at 48 kHz
    const std::size_t hops = frames / hop;
    if (hops < 64) {
        return result;  // shorter than a few beats
    }

    // The onset strength: how much the loudness (of the sound and of its changes, which favour
    // the attacks of drums) rises from one hop to the next.
    std::vector<double> onset(hops, 0.0);
    double previous_level = 0.0;
    double previous_bright = 0.0;
    float last = 0.0f;
    for (std::size_t h = 0; h < hops; ++h) {
        double energy = 0.0;
        double bright = 0.0;
        for (std::size_t i = h * hop; i < (h + 1) * hop; ++i) {
            float mono = 0.0f;
            for (std::size_t c = 0; c < channels; ++c) {
                mono += interleaved[i * channels + c];
            }
            mono /= static_cast<float>(channels);
            energy += static_cast<double>(mono) * mono;
            const double change = static_cast<double>(mono - last);
            bright += change * change;
            last = mono;
        }
        const double level = std::log1p(1000.0 * energy / static_cast<double>(hop));
        const double brightness = std::log1p(4000.0 * bright / static_cast<double>(hop));
        onset[h] = h == 0 ? 0.0
                          : std::max(0.0, level - previous_level) + std::max(0.0, brightness - previous_bright);
        previous_level = level;
        previous_bright = brightness;
    }
    // Keep only what stands above the local average.
    {
        std::vector<double> clean(hops, 0.0);
        const std::size_t half = 8;
        for (std::size_t h = 0; h < hops; ++h) {
            const std::size_t low = h >= half ? h - half : 0;
            const std::size_t high = std::min(hops - 1, h + half);
            double sum = 0.0;
            for (std::size_t k = low; k <= high; ++k) {
                sum += onset[k];
            }
            clean[h] = std::max(0.0, onset[h] - sum / static_cast<double>(high - low + 1));
        }
        onset.swap(clean);
    }
    // The clear onsets: local peaks of at least a third of the strongest one.
    const double strongest = *std::max_element(onset.begin(), onset.end());
    if (strongest < 0.05) {
        return result;  // nothing is struck
    }
    std::vector<std::size_t> peaks;
    for (std::size_t h = 1; h + 1 < hops; ++h) {
        if (onset[h] >= strongest / 3.0 && onset[h] >= onset[h - 1] && onset[h] > onset[h + 1]) {
            peaks.push_back(h);
        }
    }
    if (peaks.size() < 4) {
        return result;
    }

    // A little blur, so that periods between two whole hops are not missed.
    std::vector<double> soft(hops, 0.0);
    for (std::size_t h = 1; h + 1 < hops; ++h) {
        soft[h] = 0.25 * onset[h - 1] + 0.5 * onset[h] + 0.25 * onset[h + 1];
    }
    // How strongly the onsets repeat after every lag.
    const double hops_per_second = sample_rate / static_cast<double>(hop);
    const auto lag_of = [&](double bpm) { return 60.0 * hops_per_second / bpm; };
    const std::size_t most = std::min(hops - 1, static_cast<std::size_t>(std::ceil(4.0 * lag_of(60.0))) + 2);
    std::vector<double> correlation(most + 1, 0.0);
    for (std::size_t lag = 1; lag <= most; ++lag) {
        double sum = 0.0;
        for (std::size_t h = lag; h < hops; ++h) {
            sum += soft[h] * soft[h - lag];
        }
        correlation[lag] = sum / static_cast<double>(hops - lag);
    }
    const auto at = [&](double lag) {
        const auto below = static_cast<std::size_t>(std::floor(lag));
        if (below + 1 > most || below < 1) {
            return 0.0;
        }
        const double t = lag - static_cast<double>(below);
        return correlation[below] * (1.0 - t) + correlation[below + 1] * t;
    };
    double best_score = 0.0;
    double best_lag = 0.0;
    double total = 0.0;
    int counted = 0;
    const auto first = static_cast<std::size_t>(std::floor(lag_of(200.0)));
    const auto last_lag = static_cast<std::size_t>(std::ceil(lag_of(60.0)));
    for (std::size_t lag = std::max<std::size_t>(first, 2); lag <= last_lag && lag + 1 <= most; ++lag) {
        const double l = static_cast<double>(lag);
        // The beat repeats at twice and four times the period too; tempos near 125 are likelier.
        const double octaves = std::log2(60.0 * hops_per_second / l / 125.0);
        const double weight = std::exp(-0.5 * octaves * octaves);
        const double score = (correlation[lag] + 0.5 * at(2.0 * l) + 0.25 * at(4.0 * l)) * (0.5 + weight);
        total += score;
        ++counted;
        if (score > best_score) {
            best_score = score;
            best_lag = l;
        }
    }
    if (best_lag <= 0.0 || counted == 0 || best_score <= 0.0) {
        return result;
    }
    double period = best_lag;
    {
        const auto lag = static_cast<std::size_t>(best_lag);
        const double a = correlation[lag - 1];
        const double b = correlation[lag];
        const double c = correlation[lag + 1];
        if (a - 2.0 * b + c < 0.0) {
            period += 0.5 * (a - c) / (a - 2.0 * b + c);
        }
    }

    // Where the beats fall: the circular mean of the onsets around the period ...
    double sine = 0.0;
    double cosine = 0.0;
    for (const std::size_t h : peaks) {
        const double angle = kTwoPi * static_cast<double>(h) / period;
        sine += onset[h] * std::sin(angle);
        cosine += onset[h] * std::cos(angle);
    }
    double phase = std::atan2(sine, cosine) / kTwoPi * period;
    // ... then the beats that the onsets belong to, fitted with a straight line (beat number to
    // time), which fixes the period and the phase precisely over the whole recording.
    for (int pass = 0; pass < 3; ++pass) {
        double sk = 0.0;
        double sh = 0.0;
        double skk = 0.0;
        double skh = 0.0;
        double n = 0.0;
        for (const std::size_t h : peaks) {
            const double time = static_cast<double>(h);
            const double beat = std::round((time - phase) / period);
            if (std::fabs(time - (phase + beat * period)) > 0.2 * period) {
                continue;  // an onset between the beats
            }
            sk += beat;
            sh += time;
            skk += beat * beat;
            skh += beat * time;
            n += 1.0;
        }
        const double denominator = n * skk - sk * sk;
        if (n < 4.0 || std::fabs(denominator) < 1e-9) {
            break;
        }
        const double fitted = (n * skh - sk * sh) / denominator;
        if (!(fitted > 0.8 * period && fitted < 1.25 * period)) {
            break;
        }
        period = fitted;
        phase = (sh - period * sk) / n;
    }
    result.bpm = 60.0 * hops_per_second / period;
    result.confidence = best_score / (total / counted);
    // The rise of an onset shows in the hop where it begins: its start is the beat.
    const double beat_frames = period * static_cast<double>(hop);
    double first_beat = std::fmod(phase * static_cast<double>(hop), beat_frames);
    if (first_beat < 0.0) {
        first_beat += beat_frames;
    }
    result.first_beat = first_beat;
    return result;
}

}  // namespace toh::core
