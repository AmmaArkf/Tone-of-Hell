#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "toh/core/clip.h"

namespace toh::core {

/// The stock sounds of the library: drums, percussion, effects and short tones that Tone of
/// Hell synthesizes itself, so they belong to the project (MIT) and need no files.
struct StockSoundInfo {
    const char* category;  ///< English, for example "Drums" (pass it through the translation)
    const char* name;      ///< English, for example "Kick"
};

/// How many stock sounds there are.
std::size_t stock_sound_count() noexcept;
/// The category and name of one (index below stock_sound_count()).
StockSoundInfo stock_sound_info(std::size_t index) noexcept;
/// Synthesizes one. The same index always gives the same sound. Not real-time safe.
std::shared_ptr<const ClipData> make_stock_sound(std::size_t index, std::uint32_t sample_rate,
                                                 std::uint32_t channels);

}  // namespace toh::core
