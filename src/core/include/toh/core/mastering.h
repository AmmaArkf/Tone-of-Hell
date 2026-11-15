#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace toh::core {

/// What a finished mix measures.
struct MixStats {
    double loudness_lufs = -70.0;  ///< integrated loudness (ITU-R BS.1770, gated), -70 for silence
    double peak_db = -120.0;       ///< the loudest sample, dB of full scale
    double low_db = 0.0;           ///< the share of the lows (below 150 Hz) in dB of the whole
    double high_db = 0.0;          ///< the share of the highs (above 6 kHz)
    double width = 0.0;            ///< the side signal compared with the middle: 0 mono, 1 very wide
};

/// Measures interleaved audio (any number of channels; width needs two).
MixStats measure_mix(const float* interleaved, std::size_t frames, std::size_t channels, double sample_rate);

/// The settings of the mastering chain: a bass and a treble shelf, a gentle compressor that
/// glues the mix, the gain that reaches the target loudness and a limiter that holds the peaks
/// under the ceiling.
struct MasterSettings {
    double target_lufs = -14.0;
    double ceiling_db = -1.0;
    double low_shelf_db = 0.0;   ///< at 120 Hz, -8 to 8
    double high_shelf_db = 0.0;  ///< at 8 kHz, -8 to 8
    double glue = 0.2;           ///< 0 (none) to 1 (strong)
};

/// Suggestions for a mix and a target loudness, with the reasons in English (one per line).
MasterSettings suggest_master(const MixStats& stats, double target_lufs, std::vector<std::string>* reasons = nullptr);

/// Masters interleaved audio in place. The limiter looks 5 ms ahead, so it holds the ceiling
/// without clicks. Not real-time safe (it works on the whole piece at once).
void master_audio(std::vector<float>& interleaved, std::size_t channels, double sample_rate,
                  const MasterSettings& settings);

}  // namespace toh::core
