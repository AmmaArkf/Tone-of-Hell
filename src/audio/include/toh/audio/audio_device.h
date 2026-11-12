#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace toh::audio {

namespace detail {
struct DeviceImpl;
}

struct DeviceConfig {
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 2;
    /// Size of one audio block. Smaller means lower latency and less safety margin.
    std::uint32_t buffer_frames = 256;
};

/// Called on the audio thread to fill `out_interleaved` with `frames` frames of 32-bit float
/// samples. It must follow the real-time rules in CONTRIBUTING.md: no allocation, no locks, no
/// exceptions, no file or console access.
using RenderCallback = void (*)(void* user_data, float* out_interleaved, std::uint32_t frames);

/// An output stream to the default sound device (WASAPI on Windows, ALSA/PulseAudio/JACK on Linux).
class Device {
public:
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    /// Opens the default output device and starts calling `callback`. Returns false on failure;
    /// `error` (if given) receives a short description.
    bool start(const DeviceConfig& config, RenderCallback callback, void* user_data,
               std::string* error = nullptr);

    /// Stops the stream. When this returns, the callback is no longer running.
    void stop();

    bool running() const noexcept { return impl_ != nullptr; }
    const DeviceConfig& config() const noexcept { return config_; }

private:
    std::unique_ptr<detail::DeviceImpl> impl_;
    DeviceConfig config_;
};

}  // namespace toh::audio
