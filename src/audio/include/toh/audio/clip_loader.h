#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

#include "toh/core/clip.h"

namespace toh::audio {

/// Decodes an audio file (WAV, FLAC or MP3) completely into memory, converting it to 32-bit
/// float with the given sample rate and channel count.
///
/// Not real-time safe: call it from a normal thread, never from the audio callback. Files
/// longer than 30 minutes are refused for now (streaming from disk comes later).
/// Returns nullptr on failure; `error` (if given) receives a short description.
std::shared_ptr<const core::ClipData> load_clip(const std::filesystem::path& path,
                                                std::uint32_t sample_rate,
                                                std::uint32_t channels,
                                                std::string* error = nullptr);

/// Decodes an audio file block by block, converted like load_clip, and hands every block of
/// interleaved frames to `sink`, so that the file never has to fit in memory. Stops early when
/// `sink` returns false. Returns the number of frames handed over, or -1 when the file cannot
/// be opened or read (`error`, if given, says why). There is no length limit.
std::int64_t decode_stream(const std::filesystem::path& path, std::uint32_t sample_rate,
                           std::uint32_t channels,
                           const std::function<bool(const float*, std::size_t)>& sink,
                           std::string* error = nullptr);

}  // namespace toh::audio
