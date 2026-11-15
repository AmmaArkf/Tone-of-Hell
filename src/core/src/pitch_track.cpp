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

std::vector<float> psola_retune(const float* in, std::size_t frames, std::size_t channels, double rate,
                                const PitchCurve& curve, const std::vector<float>& shift) {
    std::vector<float> out(in, in + frames * channels);
    if (channels == 0 || frames == 0 || rate <= 0.0) {
        return out;
    }
    const std::size_t hop = std::max<std::size_t>(curve.hop, 1);
    const std::size_t hops = std::min(curve.midi.size(), shift.size());
    const auto period_of = [&](std::size_t h) {
        return rate / (440.0 * std::pow(2.0, (static_cast<double>(curve.midi[h]) - 69.0) / 12.0));
    };
    // The shift at a frame: between the middles of two hops it moves in a straight line.
    const auto shift_at = [&](double frame) {
        const double x = std::clamp(frame / static_cast<double>(hop) - 0.5, 0.0, static_cast<double>(hops - 1));
        const auto low = static_cast<std::size_t>(std::floor(x));
        const std::size_t high = std::min(low + 1, hops - 1);
        const double t = x - static_cast<double>(low);
        return static_cast<double>(shift[low]) * (1.0 - t) + static_cast<double>(shift[high]) * t;
    };
    const auto mono = [&](std::int64_t frame) {
        float sum = 0.0f;
        for (std::size_t c = 0; c < channels; ++c) {
            sum += in[static_cast<std::size_t>(frame) * channels + c];
        }
        return sum;
    };
    const auto total = static_cast<std::int64_t>(frames);
    std::size_t h = 0;
    while (h < hops) {
        if (curve.midi[h] < 0.0f) {
            ++h;
            continue;
        }
        // A run of hops with a pitch, and whether anything in it moves.
        std::size_t end = h;
        bool moves = false;
        while (end < hops && curve.midi[end] >= 0.0f) {
            moves = moves || std::fabs(shift[end]) > 0.005f;
            ++end;
        }
        const auto a = static_cast<std::int64_t>(h * hop);
        const std::int64_t b = std::min(static_cast<std::int64_t>(end * hop), total);
        h = end;
        if (!moves || b - a < 64) {
            continue;
        }
        // The marks: one at the peak of every period.
        const auto refine = [&](std::int64_t guess, double period) {
            const auto reach = static_cast<std::int64_t>(period * 0.2);
            std::int64_t best = guess;
            float highest = -1e30f;
            for (std::int64_t t = std::max(guess - reach, a); t <= std::min(guess + reach, b - 1); ++t) {
                const float value = mono(t);
                if (value > highest) {
                    highest = value;
                    best = t;
                }
            }
            return best;
        };
        std::vector<std::int64_t> marks;
        std::vector<double> periods;
        double period = period_of(static_cast<std::size_t>(a) / hop);
        std::int64_t mark = refine(a + static_cast<std::int64_t>(period * 0.5), period);
        while (mark < b) {
            const std::size_t at = std::min(static_cast<std::size_t>(mark) / hop, hops - 1);
            if (curve.midi[at] >= 0.0f) {
                period = period_of(at);
            }
            marks.push_back(mark);
            periods.push_back(period);
            std::int64_t next = refine(mark + std::llround(period), period);
            if (next <= mark) {
                next = mark + std::max<std::int64_t>(std::llround(period), 1);
            }
            mark = next;
        }
        if (marks.size() < 3) {
            continue;
        }
        // Laid down again: a piece of two periods at every new mark, the new marks a period
        // divided by the ratio apart; the sum is divided by the sum of the windows.
        const auto span = static_cast<std::size_t>(b - a);
        std::vector<double> sum(span * channels, 0.0);
        std::vector<double> weight(span, 0.0);
        double place = static_cast<double>(marks.front());
        std::size_t nearest = 0;
        while (place < static_cast<double>(b)) {
            while (nearest + 1 < marks.size() &&
                   std::fabs(static_cast<double>(marks[nearest + 1]) - place) <
                       std::fabs(static_cast<double>(marks[nearest]) - place)) {
                ++nearest;
            }
            const std::int64_t source = marks[nearest];
            const double p = periods[nearest];
            const auto half = std::max<std::int64_t>(std::llround(p), 2);
            const auto centre = static_cast<std::int64_t>(std::llround(place));
            for (std::int64_t j = -half; j <= half; ++j) {
                const std::int64_t from = source + j;
                const std::int64_t to = centre + j;
                if (from < 0 || from >= total || to < a || to >= b) {
                    continue;
                }
                const double w = 0.5 + 0.5 * std::cos(3.14159265358979 * static_cast<double>(j) / static_cast<double>(half));
                const auto index = static_cast<std::size_t>(to - a);
                for (std::size_t c = 0; c < channels; ++c) {
                    sum[index * channels + c] += static_cast<double>(in[static_cast<std::size_t>(from) * channels + c]) * w;
                }
                weight[index] += w;
            }
            const double ratio = std::pow(2.0, shift_at(place) / 12.0);
            place += p / ratio;
        }
        // Written back, blended with the original over 5 ms at the edges of the run.
        const auto fade = std::max<std::int64_t>(static_cast<std::int64_t>(0.005 * rate), 1);
        for (std::int64_t t = a; t < b; ++t) {
            const auto index = static_cast<std::size_t>(t - a);
            if (weight[index] < 1e-3) {
                continue;
            }
            const double edge = std::min({1.0, static_cast<double>(t - a) / static_cast<double>(fade),
                                          static_cast<double>(b - 1 - t) / static_cast<double>(fade)});
            for (std::size_t c = 0; c < channels; ++c) {
                const std::size_t k = static_cast<std::size_t>(t) * channels + c;
                const double moved = sum[index * channels + c] / weight[index];
                out[k] = static_cast<float>(in[k] + (moved - in[k]) * edge);
            }
        }
    }
    return out;
}

std::vector<PitchNote> find_pitch_notes(const PitchCurve& curve, float tolerance) {
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
        if (std::fabs(pitch - centre) > tolerance) {
            bool holds = true;
            for (std::size_t k = h + 1; k < std::min(h + 3, hops); ++k) {
                if (curve.midi[k] < 0.0f || std::fabs(curve.midi[k] - pitch) > tolerance) {
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
