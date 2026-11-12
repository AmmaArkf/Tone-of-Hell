#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace toh::audio {

namespace detail {
struct DeviceImpl;
}

struct DeviceConfig {
    std::uint32_t sample_rate = 48000;
    std::uint32_t channels = 2;
    /// Size of one audio block. Smaller means lower latency and less safety margin.
    std::uint32_t buffer_frames = 256;
    /// Also open an input device (microphone or line input), for recording.
    bool capture = false;
    /// Names of the devices to use, as returned by list_devices(). An empty name, or a name
    /// that no longer exists, means the system's default device.
    std::string output_name;
    std::string input_name;
};

/// The sound devices the system offers right now (names in UTF-8).
struct DeviceList {
    std::vector<std::string> outputs;
    std::vector<std::string> inputs;
};

/// Asks the system for its sound devices. Not real-time safe; may take a moment.
DeviceList list_devices();

/// Called on the audio thread. `out_interleaved` must be filled with `frames` frames of 32-bit
/// float samples. `in_interleaved` holds the same number of input frames with the same channel
/// count, or is null when no input device is open. The callback must follow the real-time
/// rules in CONTRIBUTING.md: no allocation, no locks, no exceptions, no file or console access.
using AudioCallback = void (*)(void* user_data, const float* in_interleaved,
                               float* out_interleaved, std::uint32_t frames);

/// A stream to the sound devices (WASAPI on Windows, ALSA/PulseAudio/JACK on Linux).
class Device {
public:
    Device();
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    /// Opens the output device (and, with `config.capture`, the input device) and starts
    /// calling `callback`. If the input cannot be opened, output still starts and has_input()
    /// is false. Returns false when not even output works; `error` (if given) receives a short
    /// description.
    bool start(const DeviceConfig& config, AudioCallback callback, void* user_data,
               std::string* error = nullptr);

    /// Stops the stream. When this returns, the callback is no longer running.
    void stop();

    bool running() const noexcept { return impl_ != nullptr; }
    /// True when an input device is open and the callback receives input.
    bool has_input() const noexcept { return impl_ != nullptr && has_input_; }
    const DeviceConfig& config() const noexcept { return config_; }

private:
    std::unique_ptr<detail::DeviceImpl> impl_;
    DeviceConfig config_;
    bool has_input_ = false;
};

}  // namespace toh::audio
