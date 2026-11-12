#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace toh::audio {

/// How samples are stored in a written file.
enum class SampleFormat {
    Int16,    ///< 16-bit integers: the CD format, smallest files.
    Int24,    ///< 24-bit integers: the usual choice for music production.
    Float32,  ///< 32-bit float: keeps levels above full scale.
};

/// Writes a WAV file piece by piece. Not real-time safe: use it on a normal thread.
class WavWriter {
public:
    WavWriter();
    ~WavWriter();  // closes the file if it is still open
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    /// Creates (or replaces) the file. Returns false on failure; `error` (if given) receives a
    /// short description.
    bool open(const std::filesystem::path& path, std::uint32_t sample_rate, std::uint32_t channels,
              SampleFormat format, std::string* error = nullptr);

    /// Appends interleaved 32-bit float frames. For the integer formats, values outside
    /// -1..1 are limited to that range. Returns false if the data could not be written.
    bool write(const float* interleaved, std::size_t frames);

    /// Finishes the file. Returns false if it was not open.
    bool close();

    bool is_open() const noexcept { return impl_ != nullptr; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace toh::audio
