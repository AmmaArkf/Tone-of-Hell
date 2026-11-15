#include "toh/core/mastering.h"

#include <algorithm>
#include <cmath>
#include <deque>

namespace toh::core {
namespace {

constexpr double kPi = 3.14159265358979323846;

// A second-order filter (direct form I) for one channel.
struct Biquad {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
    double x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0;

    double run(double x) noexcept {
        const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

// The shelves and passes of the "Audio EQ Cookbook" (R. Bristow-Johnson).
Biquad make_shelf(bool high, double frequency, double gain_db, double rate, double q = 0.7071) {
    const double a = std::pow(10.0, gain_db / 40.0);
    const double w = 2.0 * kPi * frequency / rate;
    const double cs = std::cos(w);
    const double alpha = std::sin(w) / (2.0 * q);
    const double root = 2.0 * std::sqrt(a) * alpha;
    Biquad f;
    double a0 = 1.0;
    if (high) {
        f.b0 = a * ((a + 1) + (a - 1) * cs + root);
        f.b1 = -2 * a * ((a - 1) + (a + 1) * cs);
        f.b2 = a * ((a + 1) + (a - 1) * cs - root);
        a0 = (a + 1) - (a - 1) * cs + root;
        f.a1 = 2 * ((a - 1) - (a + 1) * cs);
        f.a2 = (a + 1) - (a - 1) * cs - root;
    } else {
        f.b0 = a * ((a + 1) - (a - 1) * cs + root);
        f.b1 = 2 * a * ((a - 1) - (a + 1) * cs);
        f.b2 = a * ((a + 1) - (a - 1) * cs - root);
        a0 = (a + 1) + (a - 1) * cs + root;
        f.a1 = -2 * ((a - 1) + (a + 1) * cs);
        f.a2 = (a + 1) + (a - 1) * cs - root;
    }
    f.b0 /= a0;
    f.b1 /= a0;
    f.b2 /= a0;
    f.a1 /= a0;
    f.a2 /= a0;
    return f;
}

Biquad make_pass(bool high, double frequency, double rate, double q = 0.7071) {
    const double w = 2.0 * kPi * frequency / rate;
    const double cs = std::cos(w);
    const double alpha = std::sin(w) / (2.0 * q);
    Biquad f;
    const double a0 = 1.0 + alpha;
    if (high) {
        f.b0 = (1.0 + cs) / 2.0 / a0;
        f.b1 = -(1.0 + cs) / a0;
        f.b2 = (1.0 + cs) / 2.0 / a0;
    } else {
        f.b0 = (1.0 - cs) / 2.0 / a0;
        f.b1 = (1.0 - cs) / a0;
        f.b2 = (1.0 - cs) / 2.0 / a0;
    }
    f.a1 = -2.0 * cs / a0;
    f.a2 = (1.0 - alpha) / a0;
    return f;
}

// The integrated loudness of BS.1770: K-weighting, 400 ms blocks with 75 % overlap, an
// absolute gate at -70 LUFS and a relative gate 10 LU below the first average.
double integrated_loudness(const float* data, std::size_t frames, std::size_t channels, double rate) {
    std::vector<double> power;  // per 100 ms step: the K-weighted mean square summed over the channels
    const auto step = static_cast<std::size_t>(rate * 0.1);
    if (step == 0 || frames < 4 * step) {
        return -70.0;
    }
    std::vector<Biquad> shelf(channels, make_shelf(true, 1681.97, 4.0, rate, 0.7072));
    std::vector<Biquad> pass(channels, make_pass(true, 38.13, rate, 0.5003));
    double sum = 0.0;
    std::size_t count = 0;
    for (std::size_t i = 0; i < frames; ++i) {
        for (std::size_t c = 0; c < channels; ++c) {
            const double y = pass[c].run(shelf[c].run(data[i * channels + c]));
            sum += y * y;  // the surround channels would weigh more; a stereo mix has none
        }
        if (++count == step) {
            power.push_back(sum / static_cast<double>(step));
            sum = 0.0;
            count = 0;
        }
    }
    std::vector<double> blocks;
    for (std::size_t b = 0; b + 4 <= power.size(); ++b) {
        blocks.push_back((power[b] + power[b + 1] + power[b + 2] + power[b + 3]) / 4.0);
    }
    const auto lufs = [](double mean_square) { return -0.691 + 10.0 * std::log10(std::max(mean_square, 1e-12)); };
    double total = 0.0;
    std::size_t kept = 0;
    for (const double block : blocks) {
        if (lufs(block) > -70.0) {
            total += block;
            ++kept;
        }
    }
    if (kept == 0) {
        return -70.0;
    }
    const double relative = lufs(total / static_cast<double>(kept)) - 10.0;
    total = 0.0;
    kept = 0;
    for (const double block : blocks) {
        if (lufs(block) > -70.0 && lufs(block) > relative) {
            total += block;
            ++kept;
        }
    }
    return kept == 0 ? -70.0 : lufs(total / static_cast<double>(kept));
}

void limit(std::vector<float>& data, std::size_t channels, double rate, float ceiling);

}  // namespace

MixStats measure_mix(const float* data, std::size_t frames, std::size_t channels, double rate) {
    MixStats stats;
    if (data == nullptr || channels == 0 || frames == 0 || rate <= 0.0) {
        return stats;
    }
    stats.loudness_lufs = integrated_loudness(data, frames, channels, rate);
    float peak = 0.0f;
    for (std::size_t i = 0; i < frames * channels; ++i) {
        peak = std::max(peak, std::fabs(data[i]));
    }
    stats.peak_db = peak > 0.0f ? 20.0 * std::log10(static_cast<double>(peak)) : -120.0;

    // The lows and the highs of the middle signal.
    Biquad low1 = make_pass(false, 150.0, rate);
    Biquad low2 = make_pass(false, 150.0, rate);
    Biquad high1 = make_pass(true, 6000.0, rate);
    Biquad high2 = make_pass(true, 6000.0, rate);
    double all = 0.0, lows = 0.0, highs = 0.0, middle = 0.0, side = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        double mono = 0.0;
        for (std::size_t c = 0; c < channels; ++c) {
            mono += data[i * channels + c];
        }
        mono /= static_cast<double>(channels);
        all += mono * mono;
        const double l = low2.run(low1.run(mono));
        const double h = high2.run(high1.run(mono));
        lows += l * l;
        highs += h * h;
        if (channels >= 2) {
            const double m = 0.5 * (data[i * channels] + data[i * channels + 1]);
            const double s = 0.5 * (data[i * channels] - data[i * channels + 1]);
            middle += m * m;
            side += s * s;
        }
    }
    if (all > 0.0) {
        stats.low_db = 10.0 * std::log10(std::max(lows / all, 1e-9));
        stats.high_db = 10.0 * std::log10(std::max(highs / all, 1e-9));
    }
    stats.width = middle > 0.0 ? std::sqrt(side / middle) : 0.0;
    return stats;
}

MasterSettings suggest_master(const MixStats& stats, double target_lufs, std::vector<std::string>* reasons) {
    MasterSettings settings;
    settings.target_lufs = target_lufs;
    settings.ceiling_db = target_lufs > -11.0 ? -0.5 : -1.0;
    const auto say = [&](const std::string& text) {
        if (reasons != nullptr) {
            reasons->push_back(text);
        }
    };
    // Typical finished mixes: the lows about 4 dB below the whole, the highs about 16 dB below.
    constexpr double kLows = -4.0;
    constexpr double kHighs = -16.0;
    settings.low_shelf_db = std::clamp(0.6 * (kLows - stats.low_db), -4.0, 4.0);
    settings.high_shelf_db = std::clamp(0.6 * (kHighs - stats.high_db), -4.0, 4.0);
    if (settings.low_shelf_db > 0.5) {
        say("The lows are weaker than in most finished mixes: a little more bass.");
    } else if (settings.low_shelf_db < -0.5) {
        say("The lows are stronger than in most finished mixes: a little less bass.");
    }
    if (settings.high_shelf_db > 0.5) {
        say("The highs are weaker than in most finished mixes: a little more air.");
    } else if (settings.high_shelf_db < -0.5) {
        say("The highs are stronger than in most finished mixes: a little less treble.");
    }
    const double crest = stats.peak_db - stats.loudness_lufs;  // how far the peaks stick out
    if (crest > 18.0) {
        settings.glue = 0.45;
        say("The peaks stick out far above the rest: the compressor holds the mix together more.");
    } else if (crest > 14.0) {
        settings.glue = 0.25;
    } else {
        settings.glue = 0.1;
        say("The mix is already dense: the compressor stays gentle.");
    }
    const double gain = target_lufs - stats.loudness_lufs;
    if (gain > 0.5) {
        say("The mix is quieter than the target: it is raised and the limiter catches the peaks.");
    } else if (gain < -0.5) {
        say("The mix is louder than the target: it is turned down.");
    }
    if (stats.width > 0.9) {
        say("The mix is very wide: check how it sounds in mono.");
    }
    return settings;
}

void master_audio(std::vector<float>& data, std::size_t channels, double rate, const MasterSettings& settings) {
    if (channels == 0 || data.size() < channels || rate <= 0.0) {
        return;
    }
    const std::size_t frames = data.size() / channels;

    // 1. The shelves.
    if (std::fabs(settings.low_shelf_db) > 0.01 || std::fabs(settings.high_shelf_db) > 0.01) {
        std::vector<Biquad> low(channels, make_shelf(false, 120.0, std::clamp(settings.low_shelf_db, -8.0, 8.0), rate));
        std::vector<Biquad> high(channels, make_shelf(true, 8000.0, std::clamp(settings.high_shelf_db, -8.0, 8.0), rate));
        for (std::size_t i = 0; i < frames; ++i) {
            for (std::size_t c = 0; c < channels; ++c) {
                float& value = data[i * channels + c];
                value = static_cast<float>(high[c].run(low[c].run(value)));
            }
        }
    }

    // 2. The glue: a slow RMS compressor, 1.5:1 to 3.5:1, from about 6 dB above the loudness.
    const double glue = std::clamp(settings.glue, 0.0, 1.0);
    if (glue > 0.001) {
        const double loudness = integrated_loudness(data.data(), frames, channels, rate);
        const double threshold = loudness + 6.0 - 8.0 * glue;
        const double ratio = 1.5 + 2.0 * glue;
        const double attack = std::exp(-1.0 / (0.010 * rate));
        const double release = std::exp(-1.0 / (0.150 * rate));
        double level = 0.0;  // mean square
        double gain_db = 0.0;
        for (std::size_t i = 0; i < frames; ++i) {
            double square = 0.0;
            for (std::size_t c = 0; c < channels; ++c) {
                const double v = data[i * channels + c];
                square += v * v;
            }
            square /= static_cast<double>(channels);
            const double coef = square > level ? attack : release;
            level = coef * level + (1.0 - coef) * square;
            const double db = 10.0 * std::log10(std::max(level, 1e-12)) + 3.0;  // RMS of a sine reads like its loudness
            const double wanted = db > threshold ? -(db - threshold) * (1.0 - 1.0 / ratio) : 0.0;
            gain_db += (wanted - gain_db) * (wanted < gain_db ? 1.0 - attack : 1.0 - release);
            const auto gain = static_cast<float>(std::pow(10.0, gain_db / 20.0));
            for (std::size_t c = 0; c < channels; ++c) {
                data[i * channels + c] *= gain;
            }
        }
    }

    // 3. The gain to the target loudness and 4. the limiter. The limiter takes some loudness
    // away again, so the gain is raised and the two run once more until the target is met.
    const auto ceiling = static_cast<float>(std::pow(10.0, std::clamp(settings.ceiling_db, -12.0, 0.0) / 20.0));
    const double start = integrated_loudness(data.data(), frames, channels, rate);
    if (start <= -69.0) {
        for (float& value : data) {
            value = std::clamp(value, -ceiling, ceiling);
        }
        return;
    }
    const std::vector<float> before = data;
    double gain_db = settings.target_lufs - start;
    for (int pass = 0; pass < 6; ++pass) {
        const auto gain = static_cast<float>(std::pow(10.0, std::min(gain_db, 36.0) / 20.0));
        for (std::size_t k = 0; k < data.size(); ++k) {
            data[k] = before[k] * gain;
        }
        limit(data, channels, rate, ceiling);
        const double reached = integrated_loudness(data.data(), frames, channels, rate);
        if (std::fabs(settings.target_lufs - reached) < 0.2 || gain_db >= 36.0) {
            break;
        }
        gain_db += settings.target_lufs - reached;
    }
}

namespace {

// The limiter: the gain each frame needs, the lowest of it within 5 ms on either side, averaged
// over 5 ms (so it is never above what any frame needs), with a 60 ms release.
void limit(std::vector<float>& data, std::size_t channels, double rate, float ceiling) {
    const std::size_t frames = data.size() / channels;
    const std::size_t look = std::max<std::size_t>(static_cast<std::size_t>(0.005 * rate), 1);
    std::vector<float> need(frames, 1.0f);
    bool any = false;
    for (std::size_t i = 0; i < frames; ++i) {
        float peak = 0.0f;
        for (std::size_t c = 0; c < channels; ++c) {
            peak = std::max(peak, std::fabs(data[i * channels + c]));
        }
        if (peak > ceiling) {
            need[i] = ceiling / peak;
            any = true;
        }
    }
    if (any) {
        std::vector<float> lowest(frames, 1.0f);
        std::deque<std::size_t> window;  // indices with rising values: the front is the minimum
        std::size_t next = 0;
        for (std::size_t i = 0; i < frames; ++i) {
            const std::size_t last = std::min(frames - 1, i + look);
            while (next <= last) {
                while (!window.empty() && need[window.back()] >= need[next]) {
                    window.pop_back();
                }
                window.push_back(next++);
            }
            while (window.front() + look < i) {
                window.pop_front();
            }
            lowest[i] = need[window.front()];
        }
        const std::size_t half = look / 2;
        std::vector<double> prefix(frames + 1, 0.0);
        for (std::size_t i = 0; i < frames; ++i) {
            prefix[i + 1] = prefix[i] + lowest[i];
        }
        const double release = 1.0 - std::exp(-1.0 / (0.060 * rate));
        double gain = 1.0;
        for (std::size_t i = 0; i < frames; ++i) {
            const std::size_t a = i >= half ? i - half : 0;
            const std::size_t b = std::min(frames, i + half + 1);
            const double smooth = (prefix[b] - prefix[a]) / static_cast<double>(b - a);
            gain = std::min(smooth, gain + (1.0 - gain) * release);
            for (std::size_t c = 0; c < channels; ++c) {
                data[i * channels + c] = static_cast<float>(data[i * channels + c] * gain);
            }
        }
    }
    // A last safety: nothing above the ceiling.
    for (float& value : data) {
        value = std::clamp(value, -ceiling, ceiling);
    }
}

}  // namespace

}  // namespace toh::core
