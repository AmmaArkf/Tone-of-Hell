#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "toh/core/notes.h"
#include "toh/core/paged_file.h"

namespace toh::core {

/// A block of audio: interleaved 32-bit float samples, either held in memory (`samples`) or
/// kept in a cache file on disk that is read in pages of 1 MB (`disk`, with `samples` empty).
///
/// A clip is immutable once it has been handed to the engine, so the audio thread can read it
/// without any locking.
struct ClipData {
    std::vector<float> samples;
    std::shared_ptr<PagedFile> disk;
    /// For note regions: the notes that the track's instrument plays (no samples then).
    std::shared_ptr<const NoteData> notes;
    std::uint32_t channels = 0;
    std::uint32_t sample_rate = 0;

    /// Number of frames (one frame holds one sample for every channel).
    std::int64_t frames() const noexcept {
        if (notes != nullptr) {
            return notes->frames;
        }
        if (disk != nullptr) {
            return disk->frames();
        }
        return channels == 0 ? 0 : static_cast<std::int64_t>(samples.size() / channels);
    }
};

/// Copies up to `count` frames starting at `first` to `out`, from memory or from the disk.
/// For normal threads only (it may wait for the disk). Returns the number of frames copied.
std::size_t read_clip_frames(const ClipData& clip, std::int64_t first, std::size_t count,
                             float* out);

/// Generates a sine tone. When `seconds * frequency_hz` is a whole number the clip loops
/// without a click. Not real-time safe (it allocates).
std::shared_ptr<const ClipData> make_sine_clip(double frequency_hz, double seconds,
                                               std::uint32_t sample_rate, std::uint32_t channels,
                                               float amplitude = 0.25f);

}  // namespace toh::core
