#include "toh/audio/audio_device.h"

#include "miniaudio.h"

namespace toh::audio {
namespace detail {

struct DeviceImpl {
    DeviceImpl() = default;
    ~DeviceImpl() {
        if (device_open) {
            ma_device_uninit(&device);  // stops the stream and waits for the callback to finish
        }
        if (context_open) {
            ma_context_uninit(&context);
        }
    }
    DeviceImpl(const DeviceImpl&) = delete;
    DeviceImpl& operator=(const DeviceImpl&) = delete;

    ma_context context{};
    bool context_open = false;
    ma_device device{};
    bool device_open = false;
    ma_device_id output_id{};
    ma_device_id input_id{};
    AudioCallback callback = nullptr;
    void* user_data = nullptr;
};

}  // namespace detail

namespace {

void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
    auto* impl = static_cast<detail::DeviceImpl*>(device->pUserData);
    impl->callback(impl->user_data, static_cast<const float*>(input), static_cast<float*>(output),
                   frame_count);
}

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

// Looks for a device by name. Returns false when the name is empty or not found.
bool find_device(ma_context& context, const std::string& name, bool input, ma_device_id& id) {
    if (name.empty()) {
        return false;
    }
    ma_device_info* outputs = nullptr;
    ma_uint32 output_count = 0;
    ma_device_info* inputs = nullptr;
    ma_uint32 input_count = 0;
    if (ma_context_get_devices(&context, &outputs, &output_count, &inputs, &input_count) !=
        MA_SUCCESS) {
        return false;
    }
    const ma_device_info* list = input ? inputs : outputs;
    const ma_uint32 count = input ? input_count : output_count;
    for (ma_uint32 i = 0; i < count; ++i) {
        if (name == list[i].name) {
            id = list[i].id;
            return true;
        }
    }
    return false;
}

// Opens and starts a stream. On failure the device is left closed.
ma_result open_device(detail::DeviceImpl& impl, const DeviceConfig& config, bool with_input) {
    ma_device_config device_config =
        ma_device_config_init(with_input ? ma_device_type_duplex : ma_device_type_playback);
    device_config.playback.format = ma_format_f32;
    device_config.playback.channels = config.channels;
    if (find_device(impl.context, config.output_name, false, impl.output_id)) {
        device_config.playback.pDeviceID = &impl.output_id;
    }
    if (with_input) {
        device_config.capture.format = ma_format_f32;
        device_config.capture.channels = config.channels;
        if (find_device(impl.context, config.input_name, true, impl.input_id)) {
            device_config.capture.pDeviceID = &impl.input_id;
        }
    }
    device_config.sampleRate = config.sample_rate;
    device_config.periodSizeInFrames = config.buffer_frames;
    device_config.dataCallback = &data_callback;
    device_config.pUserData = &impl;

    ma_result result = ma_device_init(&impl.context, &device_config, &impl.device);
    if (result != MA_SUCCESS) {
        return result;
    }
    result = ma_device_start(&impl.device);
    if (result != MA_SUCCESS) {
        ma_device_uninit(&impl.device);
        return result;
    }
    impl.device_open = true;
    return MA_SUCCESS;
}

}  // namespace

DeviceList list_devices() {
    DeviceList list;
    ma_context context;
    if (ma_context_init(nullptr, 0, nullptr, &context) != MA_SUCCESS) {
        return list;
    }
    ma_device_info* outputs = nullptr;
    ma_uint32 output_count = 0;
    ma_device_info* inputs = nullptr;
    ma_uint32 input_count = 0;
    if (ma_context_get_devices(&context, &outputs, &output_count, &inputs, &input_count) ==
        MA_SUCCESS) {
        for (ma_uint32 i = 0; i < output_count; ++i) {
            list.outputs.emplace_back(outputs[i].name);
        }
        for (ma_uint32 i = 0; i < input_count; ++i) {
            list.inputs.emplace_back(inputs[i].name);
        }
    }
    ma_context_uninit(&context);
    return list;
}

Device::Device() = default;

Device::~Device() = default;

bool Device::start(const DeviceConfig& config, AudioCallback callback, void* user_data,
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

    ma_result result = ma_context_init(nullptr, 0, nullptr, &impl->context);
    if (result != MA_SUCCESS) {
        set_error(error,
                  std::string("cannot reach the sound system: ") + ma_result_description(result));
        return false;
    }
    impl->context_open = true;

    bool with_input = config.capture;
    result = open_device(*impl, config, with_input);
    if (result != MA_SUCCESS && with_input) {
        // No usable input device: carry on with output only.
        with_input = false;
        result = open_device(*impl, config, false);
    }
    if (result != MA_SUCCESS) {
        set_error(error,
                  std::string("cannot open the audio device: ") + ma_result_description(result));
        return false;
    }

    impl_ = std::move(impl);
    config_ = config;
    has_input_ = with_input;
    return true;
}

void Device::stop() {
    impl_.reset();
    has_input_ = false;
}

}  // namespace toh::audio
