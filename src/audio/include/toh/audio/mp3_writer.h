#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace toh::audio {

/// Writes an MP3 file piece by piece with the LAME encoder (a separate, LGPL-licensed
/// library). Not real-time safe: use it on a normal thread.
class Mp3Writer {
public:
    Mp3Writer();
    ~Mp3Writer();  // finishes the file if it is still open
    Mp3Writer(const Mp3Writer&) = delete;
    Mp3Writer& operator=(const Mp3Writer&) = delete;

    /// Creates (or replaces) the file. `kbps` is a constant bit rate such as 320, 192 or 128;
    /// 0 chooses variable bit rate at the best quality. Only 1 or 2 channels are possible.
    /// Returns false on failure; `error` (if given) receives a short description.
    bool open(const std::filesystem::path& path, std::uint32_t sample_rate, std::uint32_t channels,
              int kbps, std::string* error = nullptr);

    /// Appends interleaved 32-bit float frames. Values outside -1..1 are limited to that range.
    /// Returns false if the data could not be encoded or written.
    bool write(const float* interleaved, std::size_t frames);

    /// Finishes the file: writes the last frames and the header that tells players the length.
    /// Returns false if it was not open or could not be finished.
    bool close();

    bool is_open() const noexcept { return impl_ != nullptr; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace toh::audio
