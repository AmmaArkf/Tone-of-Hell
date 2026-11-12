#include "toh/audio/audio_device.h"

#include "miniaudio.h"

namespace toh::audio {
namespace detail {

struct DeviceImpl {
    DeviceImpl() = default;
    ~DeviceImpl() {
        if (initialised) {
            ma_device_uninit(&device);  // stops the stream and waits for the callback to finish
        }
    }
    DeviceImpl(const DeviceImpl&) = delete;
    DeviceImpl& operator=(const DeviceImpl&) = delete;

    ma_device device{};
    bool initialised = false;
    RenderCallback callback = nullptr;
    void* user_data = nullptr;
};

}  // namespace detail

namespace {

void data_callback(ma_device* device, void* output, const void* /*input*/, ma_uint32 frame_count) {
    auto* impl = static_cast<detail::DeviceImpl*>(device->pUserData);
    impl->callback(impl->user_data, static_cast<float*>(output), frame_count);
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

}  // namespace

Device::Device() = default;

Device::~Device() = default;

bool Device::start(const DeviceConfig& config, RenderCallback callback, void* user_data,
                   std::string* error) {
    stop();

    if (callback == nullptr || config.sample_rate == 0 || config.channels == 0 ||
        config.buffer_frames == 0) {
        set_error(error, "invalid device configuration");
        return false;
    }

    auto impl = std::make_unique<detail::DeviceImpl>();
    impl->callback = callback;
    impl->user_data = user_data;

    ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
    device_config.playback.format = ma_format_f32;
    device_config.playback.channels = config.channels;
    device_config.sampleRate = config.sample_rate;
    device_config.periodSizeInFrames = config.buffer_frames;
    device_config.dataCallback = &data_callback;
    device_config.pUserData = impl.get();

    ma_result result = ma_device_init(nullptr, &device_config, &impl->device);
    if (result != MA_SUCCESS) {
        set_error(error,
                  std::string("cannot open the audio device: ") + ma_result_description(result));
        return false;
    }
    impl->initialised = true;

    result = ma_device_start(&impl->device);
    if (result != MA_SUCCESS) {
        set_error(error,
                  std::string("cannot start the audio device: ") + ma_result_description(result));
        return false;  // ~DeviceImpl releases the device
    }

    impl_ = std::move(impl);
    config_ = config;
    return true;
}

void Device::stop() { impl_.reset(); }

}  // namespace toh::audio
