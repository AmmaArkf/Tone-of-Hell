#include "toh/audio/clip_loader.h"

#include <cstddef>
#include <vector>

#include "miniaudio.h"

namespace toh::audio {
namespace {

constexpr std::uint64_t kMaxSeconds = 30 * 60;
constexpr ma_uint64 kChunkFrames = 4096;

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

}  // namespace

std::shared_ptr<const core::ClipData> load_clip(const std::filesystem::path& path,
                                                std::uint32_t sample_rate,
                                                std::uint32_t channels, std::string* error) {
    if (sample_rate == 0 || channels == 0) {
        set_error(error, "invalid output format");
        return nullptr;
    }

    // Asking miniaudio for f32 with our rate and channel count makes it resample and remix.
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, channels, sample_rate);
    ma_decoder decoder;
#ifdef _WIN32
    ma_result result = ma_decoder_init_file_w(path.c_str(), &config, &decoder);
#else
    ma_result result = ma_decoder_init_file(path.c_str(), &config, &decoder);
#endif
    if (result != MA_SUCCESS) {
        set_error(error,
                  std::string("cannot open or decode the file: ") + ma_result_description(result));
        return nullptr;
    }

    auto clip = std::make_shared<core::ClipData>();
    clip->channels = channels;
    clip->sample_rate = sample_rate;

    const std::uint64_t max_frames = static_cast<std::uint64_t>(sample_rate) * kMaxSeconds;
    std::vector<float> chunk(static_cast<std::size_t>(kChunkFrames) * channels);
    bool too_long = false;
    std::string read_error;

    for (;;) {
        ma_uint64 frames_read = 0;
        result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunkFrames, &frames_read);
        const std::size_t count = static_cast<std::size_t>(frames_read) * channels;
        clip->samples.insert(clip->samples.end(), chunk.begin(),
                             chunk.begin() + static_cast<std::ptrdiff_t>(count));

        if (clip->samples.size() / channels > max_frames) {
            too_long = true;
            break;
        }
        if (result == MA_AT_END) {
            break;
        }
        if (result != MA_SUCCESS) {
            read_error = ma_result_description(result);
            break;
        }
        if (frames_read == 0) {
            break;  // no progress: stop instead of looping forever
        }
    }
    ma_decoder_uninit(&decoder);

    if (too_long) {
        set_error(error, "the file is longer than 30 minutes");
        return nullptr;
    }
    if (!read_error.empty() && clip->samples.empty()) {
        set_error(error, "cannot read the file: " + read_error);
        return nullptr;
    }
    if (clip->samples.empty()) {
        set_error(error, "the file contains no audio");
        return nullptr;
    }
    return clip;
}

std::int64_t decode_stream(const std::filesystem::path& path, std::uint32_t sample_rate,
                           std::uint32_t channels,
                           const std::function<bool(const float*, std::size_t)>& sink,
                           std::string* error) {
    if (sample_rate == 0 || channels == 0) {
        set_error(error, "invalid output format");
        return -1;
    }
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, channels, sample_rate);
    ma_decoder decoder;
#ifdef _WIN32
    ma_result result = ma_decoder_init_file_w(path.c_str(), &config, &decoder);
#else
    ma_result result = ma_decoder_init_file(path.c_str(), &config, &decoder);
#endif
    if (result != MA_SUCCESS) {
        set_error(error,
                  std::string("cannot open or decode the file: ") + ma_result_description(result));
        return -1;
    }

    std::vector<float> chunk(static_cast<std::size_t>(kChunkFrames) * channels);
    std::int64_t total = 0;
    std::string read_error;
    for (;;) {
        ma_uint64 frames_read = 0;
        result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), kChunkFrames, &frames_read);
        if (frames_read > 0) {
            if (!sink(chunk.data(), static_cast<std::size_t>(frames_read))) {
                break;
            }
            total += static_cast<std::int64_t>(frames_read);
        }
        if (result == MA_AT_END) {
            break;
        }
        if (result != MA_SUCCESS) {
            read_error = ma_result_description(result);
            break;
        }
        if (frames_read == 0) {
            break;  // no progress: stop instead of looping forever
        }
    }
    ma_decoder_uninit(&decoder);

    if (!read_error.empty() && total == 0) {
        set_error(error, "cannot read the file: " + read_error);
        return -1;
    }
    return total;
}

}  // namespace toh::audio
