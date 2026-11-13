#pragma once

#include <atomic>
#include <cstdint>

namespace toh::core {

/// An insert effect of a track: it changes the sound of the track before its fader. Plugins and
/// built-in effects implement it. The engine calls process() for every block, also where no
/// region plays, so that echoes and reverbs ring out.
class AudioEffect {
public:
    AudioEffect() = default;
    virtual ~AudioEffect() = default;
    AudioEffect(const AudioEffect&) = delete;
    AudioEffect& operator=(const AudioEffect&) = delete;

    /// Audio thread: changes `frames` interleaved frames of `channels` channels in place. It
    /// must not block, lock or allocate.
    virtual void process(float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept = 0;

    /// A switched off effect is skipped: the sound passes unchanged. Any thread.
    void set_enabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> enabled_{true};
};

}  // namespace toh::core
