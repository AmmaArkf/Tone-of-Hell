#pragma once

#include <cstdint>
#include <filesystem>
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

}  // namespace toh::audio
