#include "toh/audio/wav_writer.h"

#include <vector>

#include "miniaudio.h"

namespace toh::audio {

struct WavWriter::Impl {
    ma_encoder encoder{};
    SampleFormat format = SampleFormat::Int24;
    std::uint32_t channels = 0;
    std::vector<float> limited;         // samples brought into range
    std::vector<unsigned char> packed;  // samples converted to the file format
};

namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

}  // namespace

WavWriter::WavWriter() = default;

WavWriter::~WavWriter() { close(); }

bool WavWriter::open(const std::filesystem::path& path, std::uint32_t sample_rate,
                     std::uint32_t channels, SampleFormat format, std::string* error) {
    close();
    if (sample_rate == 0 || channels == 0) {
        set_error(error, "invalid file format");
        return false;
    }

    ma_format file_format = ma_format_s24;
    if (format == SampleFormat::Int16) {
        file_format = ma_format_s16;
    } else if (format == SampleFormat::Float32) {
        file_format = ma_format_f32;
    }

    auto impl = std::make_unique<Impl>();
    impl->format = format;
    impl->channels = channels;
    const ma_encoder_config config =
        ma_encoder_config_init(ma_encoding_format_wav, file_format, channels, sample_rate);
#ifdef _WIN32
    const ma_result result = ma_encoder_init_file_w(path.c_str(), &config, &impl->encoder);
#else
    const ma_result result = ma_encoder_init_file(path.c_str(), &config, &impl->encoder);
#endif
    if (result != MA_SUCCESS) {
        set_error(error, std::string("cannot create the file: ") + ma_result_description(result));
        return false;
    }
    impl_ = std::move(impl);
    return true;
}

bool WavWriter::write(const float* interleaved, std::size_t frames) {
    if (!impl_) {
        return false;
    }
    if (frames == 0) {
        return true;
    }
    const std::size_t samples = frames * impl_->channels;
    const bool integer = impl_->format != SampleFormat::Float32;

    // Bring the samples into range for integer files; replace "not a number" by silence.
    impl_->limited.resize(samples);
    for (std::size_t i = 0; i < samples; ++i) {
        float value = interleaved[i];
        if (!(value == value)) {
            value = 0.0f;
        } else if (integer) {
            value = value > 1.0f ? 1.0f : (value < -1.0f ? -1.0f : value);
        }
        impl_->limited[i] = value;
    }

    const void* data = impl_->limited.data();
    if (impl_->format == SampleFormat::Int16) {
        impl_->packed.resize(samples * 2);
        ma_pcm_f32_to_s16(impl_->packed.data(), impl_->limited.data(), samples, ma_dither_mode_none);
        data = impl_->packed.data();
    } else if (impl_->format == SampleFormat::Int24) {
        impl_->packed.resize(samples * 3);
        ma_pcm_f32_to_s24(impl_->packed.data(), impl_->limited.data(), samples, ma_dither_mode_none);
        data = impl_->packed.data();
    }

    ma_uint64 written = 0;
    const ma_result result = ma_encoder_write_pcm_frames(&impl_->encoder, data, frames, &written);
    return result == MA_SUCCESS && written == frames;
}

bool WavWriter::close() {
    if (!impl_) {
        return false;
    }
    ma_encoder_uninit(&impl_->encoder);
    impl_.reset();
    return true;
}

}  // namespace toh::audio
