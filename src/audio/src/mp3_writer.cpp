#include "toh/audio/mp3_writer.h"

#include <algorithm>
#include <cstdio>
#include <vector>

#include <lame.h>

namespace toh::audio {

struct Mp3Writer::Impl {
    Impl() = default;
    ~Impl() {
        if (file != nullptr) {
            std::fclose(file);
        }
        if (encoder != nullptr) {
            lame_close(encoder);
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    lame_global_flags* encoder = nullptr;
    std::FILE* file = nullptr;
    std::uint32_t channels = 0;
    std::vector<float> limited;
    std::vector<unsigned char> encoded;
};

namespace {

constexpr std::size_t kChunkFrames = 4096;

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

// The size LAME asks for in its documentation: 1.25 times the samples plus 7200 bytes.
std::size_t encoded_capacity(std::size_t frames) { return frames + frames / 4 + 7200; }

std::FILE* open_for_writing(const std::filesystem::path& path) {
#ifdef _WIN32
    return _wfopen(path.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

}  // namespace

Mp3Writer::Mp3Writer() = default;

Mp3Writer::~Mp3Writer() { close(); }

bool Mp3Writer::open(const std::filesystem::path& path, std::uint32_t sample_rate,
                     std::uint32_t channels, int kbps, std::string* error) {
    close();
    if (sample_rate == 0 || channels < 1 || channels > 2 || kbps < 0 || kbps > 320) {
        set_error(error, "invalid MP3 format");
        return false;
    }

    auto impl = std::make_unique<Impl>();
    impl->channels = channels;
    impl->encoder = lame_init();
    if (impl->encoder == nullptr) {
        set_error(error, "the MP3 encoder could not be started");
        return false;
    }
    lame_global_flags* encoder = impl->encoder;
    lame_set_in_samplerate(encoder, static_cast<int>(sample_rate));
    lame_set_num_channels(encoder, static_cast<int>(channels));
    lame_set_mode(encoder, channels == 1 ? MONO : JOINT_STEREO);
    lame_set_quality(encoder, 2);        // near the best, still fast
    lame_set_bWriteVbrTag(encoder, 1);   // lets players know the exact length
    if (kbps > 0) {
        lame_set_VBR(encoder, vbr_off);
        lame_set_brate(encoder, kbps);
    } else {
        lame_set_VBR(encoder, vbr_default);
        lame_set_VBR_quality(encoder, 0.0f);  // V0: the highest quality
    }
    if (lame_init_params(encoder) < 0) {
        set_error(error, "the MP3 encoder does not accept these settings");
        return false;
    }

    impl->file = open_for_writing(path);
    if (impl->file == nullptr) {
        set_error(error, "cannot create the file");
        return false;
    }
    impl_ = std::move(impl);
    return true;
}

bool Mp3Writer::write(const float* interleaved, std::size_t frames) {
    if (!impl_) {
        return false;
    }
    const std::size_t channels = impl_->channels;
    impl_->encoded.resize(encoded_capacity(kChunkFrames));

    std::size_t done = 0;
    while (done < frames) {
        const std::size_t count = std::min(kChunkFrames, frames - done);
        const std::size_t samples = count * channels;

        // Bring the samples into range; replace "not a number" by silence.
        impl_->limited.resize(samples);
        const float* source = interleaved + done * channels;
        for (std::size_t i = 0; i < samples; ++i) {
            const float value = source[i];
            impl_->limited[i] = !(value == value) ? 0.0f : std::clamp(value, -1.0f, 1.0f);
        }

        int bytes = 0;
        if (channels == 2) {
            bytes = lame_encode_buffer_interleaved_ieee_float(
                impl_->encoder, impl_->limited.data(), static_cast<int>(count),
                impl_->encoded.data(), static_cast<int>(impl_->encoded.size()));
        } else {
            bytes = lame_encode_buffer_ieee_float(
                impl_->encoder, impl_->limited.data(), impl_->limited.data(),
                static_cast<int>(count), impl_->encoded.data(),
                static_cast<int>(impl_->encoded.size()));
        }
        if (bytes < 0) {
            return false;
        }
        const auto length = static_cast<std::size_t>(bytes);
        if (length > 0 && std::fwrite(impl_->encoded.data(), 1, length, impl_->file) != length) {
            return false;
        }
        done += count;
    }
    return true;
}

bool Mp3Writer::close() {
    if (!impl_) {
        return false;
    }
    bool ok = true;
    impl_->encoded.resize(encoded_capacity(kChunkFrames));

    // The last, incomplete frames.
    const int bytes = lame_encode_flush(impl_->encoder, impl_->encoded.data(),
                                        static_cast<int>(impl_->encoded.size()));
    if (bytes < 0) {
        ok = false;
    } else if (bytes > 0) {
        const auto length = static_cast<std::size_t>(bytes);
        ok = std::fwrite(impl_->encoded.data(), 1, length, impl_->file) == length;
    }

    // The header with the length and the encoder delay goes over the first, reserved frame.
    if (ok) {
        const std::size_t tag =
            lame_get_lametag_frame(impl_->encoder, impl_->encoded.data(), impl_->encoded.size());
        if (tag > 0 && tag <= impl_->encoded.size()) {
            ok = std::fseek(impl_->file, 0, SEEK_SET) == 0 &&
                 std::fwrite(impl_->encoded.data(), 1, tag, impl_->file) == tag;
        }
    }

    ok = std::fclose(impl_->file) == 0 && ok;
    impl_->file = nullptr;
    impl_.reset();  // closes the encoder
    return ok;
}

}  // namespace toh::audio
