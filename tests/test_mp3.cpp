// Tests for MP3 export.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/audio/clip_loader.h"
#include "toh/audio/mp3_writer.h"
#include "toh/core/clip.h"

namespace {

constexpr std::uint32_t kRate = 48000;

std::filesystem::path temp_file(const char* name) {
    return std::filesystem::temp_directory_path() / std::filesystem::u8path(name);
}

double rms(const std::vector<float>& samples, std::size_t first, std::size_t last) {
    double sum = 0.0;
    for (std::size_t i = first; i < last; ++i) {
        sum += static_cast<double>(samples[i]) * static_cast<double>(samples[i]);
    }
    return std::sqrt(sum / static_cast<double>(last - first));
}

}  // namespace

TEST_CASE("MP3 export keeps the length and the level of the project") {
    toh::app::Session session;
    // Two seconds of a 440 Hz tone at half scale: its RMS level is 0.5 / sqrt(2).
    REQUIRE(session.add_track("tone", toh::core::make_sine_clip(440.0, 2.0, kRate, 2, 0.5f)));

    for (const int kbps : {320, 128, 0}) {
        CAPTURE(kbps);
        const auto path = temp_file("toh_test_\xD1\x8D\xD0\xBA\xD1\x81\xD0\xBF\xD0\xBE\xD1\x80\xD1\x82.mp3");
        std::string error;
        REQUIRE_MESSAGE(session.export_mp3(path, kbps, &error), error);

        // Rough size check: 2 seconds at the given bit rate.
        const auto bytes = std::filesystem::file_size(path);
        if (kbps > 0) {
            const double expected = 2.0 * kbps * 1000.0 / 8.0;
            CHECK(static_cast<double>(bytes) > expected * 0.9);
            CHECK(static_cast<double>(bytes) < expected * 1.2);
        } else {
            // Variable bit rate spends very little on a pure tone; it must still be a real file.
            CHECK(bytes > 4000);
            CHECK(bytes < 2 * 320 * 1000 / 8);
        }

        const auto decoded = toh::audio::load_clip(path, kRate, 2, &error);
        REQUIRE_MESSAGE(decoded != nullptr, error);
        // The encoder adds a little padding; the header lets decoders remove most of it.
        CHECK(decoded->frames() >= 2 * 48000 - 100);
        CHECK(decoded->frames() <= 2 * 48000 + 2 * 1152 + 600);

        // Compare the level away from the edges.
        const double level = rms(decoded->samples, 2 * 20000, 2 * 70000);
        CHECK(level == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.03));
        float peak = 0.0f;
        for (const float sample : decoded->samples) {
            peak = std::max(peak, std::fabs(sample));
        }
        CHECK(peak < 0.6f);

        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
}

TEST_CASE("MP3 writer refuses what it cannot do") {
    toh::audio::Mp3Writer writer;
    std::string error;
    const auto path = temp_file("toh_test_refused.mp3");
    CHECK_FALSE(writer.open(path, kRate, 6, 320, &error));  // surround is not possible
    CHECK_FALSE(error.empty());
    CHECK_FALSE(writer.open(path, kRate, 2, 999, &error));
    CHECK_FALSE(writer.open(temp_file("no_such_folder_42/x.mp3"), kRate, 2, 320, &error));
    CHECK_FALSE(writer.is_open());
    CHECK_FALSE(writer.close());

    float silence[64] = {};
    CHECK_FALSE(writer.write(silence, 32));

    toh::app::Session empty;
    CHECK_FALSE(empty.export_mp3(path, 320, &error));
}

TEST_CASE("MP3 writer handles mono, odd block sizes and values out of range") {
    const auto path = temp_file("toh_test_mono.mp3");
    toh::audio::Mp3Writer writer;
    std::string error;
    REQUIRE_MESSAGE(writer.open(path, kRate, 1, 192, &error), error);

    std::vector<float> block(1000);
    for (int round = 0; round < 50; ++round) {
        for (std::size_t i = 0; i < block.size(); ++i) {
            block[i] = (i % 2 == 0) ? 3.0f : -3.0f;  // far above full scale
        }
        block[17] = std::nanf("");
        REQUIRE(writer.write(block.data(), 1 + static_cast<std::size_t>(round) * 37 % 1000));
    }
    REQUIRE(writer.close());
    CHECK(std::filesystem::file_size(path) > 1000);

    const auto decoded = toh::audio::load_clip(path, kRate, 1, &error);
    REQUIRE_MESSAGE(decoded != nullptr, error);
    for (const float sample : decoded->samples) {
        CHECK(std::isfinite(sample));
    }

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}
