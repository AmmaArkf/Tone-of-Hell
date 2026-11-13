// Tests for audio that is played from disk in pages of 1 MB.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/audio/wav_writer.h"
#include "toh/core/clip.h"
#include "toh/core/paged_file.h"

using toh::app::RegionInfo;
using toh::app::Session;
using toh::core::PagedFile;

namespace {

constexpr std::int64_t kPageFrames =
    static_cast<std::int64_t>(PagedFile::kPageBytes / (2 * sizeof(float)));  // stereo

// A raw stereo file in which frame n holds n on the left and -n on the right.
std::filesystem::path write_counting_file(const char* name, std::int64_t frames) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::vector<float> data(static_cast<std::size_t>(frames) * 2);
    for (std::int64_t i = 0; i < frames; ++i) {
        data[static_cast<std::size_t>(i) * 2] = static_cast<float>(i);
        data[static_cast<std::size_t>(i) * 2 + 1] = -static_cast<float>(i);
    }
    std::FILE* file = toh::core::open_file(path, "wb");
    REQUIRE(file != nullptr);
    REQUIRE(std::fwrite(data.data(), sizeof(float), data.size(), file) == data.size());
    std::fclose(file);
    return path;
}

}  // namespace

TEST_CASE("PagedFile plays only pages that the loader brought in") {
    const std::filesystem::path path = write_counting_file("toh_test_paged.f32", 2 * kPageFrames + 1000);
    {
        const auto file = PagedFile::open(path, 2);
        REQUIRE(file);
        CHECK(file->frames() == 2 * kPageFrames + 1000);
        CHECK(file->frames_per_page() == kPageFrames);
        CHECK(file->page_count() == 3);

        float frame[2] = {};
        CHECK_FALSE(file->read_frame(5, frame));  // nothing is in memory yet
        CHECK(file->resident_bytes() == 0);

        file->prefetch({1});
        CHECK(file->is_resident(1));
        CHECK_FALSE(file->is_resident(0));
        REQUIRE(file->read_frame(kPageFrames + 7, frame));
        CHECK(frame[0] == static_cast<float>(kPageFrames + 7));
        CHECK(frame[1] == -static_cast<float>(kPageFrames + 7));
        CHECK_FALSE(file->read_frame(3, frame));

        file->prefetch({2, 0});
        REQUIRE(file->read_frame(2 * kPageFrames + 999, frame));
        CHECK(frame[0] == static_cast<float>(2 * kPageFrames + 999));
        REQUIRE(file->read_frame(3, frame));
        CHECK(frame[0] == 3.0f);
        CHECK_FALSE(file->read_frame(2 * kPageFrames + 1000, frame));  // past the end
        CHECK(file->resident_bytes() == 3 * PagedFile::kPageBytes);

        // Reads from normal threads cross page boundaries and stop at the end.
        std::vector<float> block(40);
        CHECK(file->read_blocking(kPageFrames - 10, 20, block.data()) == 20);
        CHECK(block[0] == static_cast<float>(kPageFrames - 10));
        CHECK(block[38] == static_cast<float>(kPageFrames + 9));
        CHECK(file->read_blocking(2 * kPageFrames + 990, 20, block.data()) == 10);
    }
    std::filesystem::remove(path);
}

TEST_CASE("PagedFile keeps at most kSlots pages in memory") {
    const std::int64_t pages = static_cast<std::int64_t>(PagedFile::kSlots) + 3;
    const std::filesystem::path path = write_counting_file("toh_test_slots.f32", pages * kPageFrames);
    {
        const auto file = PagedFile::open(path, 2);
        REQUIRE(file);
        std::vector<std::int64_t> all;
        for (std::int64_t p = 0; p < pages; ++p) {
            all.push_back(p);
        }
        file->prefetch(all);
        CHECK(file->resident_bytes() == PagedFile::kSlots * PagedFile::kPageBytes);
        CHECK(file->is_resident(0));
        CHECK_FALSE(file->is_resident(pages - 1));

        file->prefetch({pages - 1, pages - 2});
        CHECK(file->is_resident(pages - 1));
        CHECK(file->is_resident(pages - 2));
        CHECK(file->resident_bytes() <= PagedFile::kSlots * PagedFile::kPageBytes);
        float frame[2] = {};
        REQUIRE(file->read_frame((pages - 1) * kPageFrames + 5, frame));
        CHECK(frame[0] == static_cast<float>((pages - 1) * kPageFrames + 5));
    }
    std::filesystem::remove(path);
}

TEST_CASE("Session imports a file into its cache and plays it from disk") {
    const std::int64_t frames = 3 * kPageFrames + 500;
    const std::filesystem::path wav = std::filesystem::temp_directory_path() / "toh_test_stream.wav";
    {
        toh::audio::WavWriter writer;
        REQUIRE(writer.open(wav, 48000, 2, toh::audio::SampleFormat::Float32));
        std::vector<float> block(2 * 1000);
        for (std::int64_t at = 0; at < frames; at += 1000) {
            const auto count = static_cast<std::uint32_t>(std::min<std::int64_t>(1000, frames - at));
            for (std::uint32_t i = 0; i < count; ++i) {
                const float value = static_cast<float>((at + i) % 1000) / 2000.0f + 0.1f;
                block[2 * i] = value;
                block[2 * i + 1] = value;
            }
            REQUIRE(writer.write(block.data(), count));
        }
        writer.close();
    }

    std::filesystem::path folder;
    {
        Session session;
        std::string error;
        REQUIRE_MESSAGE(session.add_track_from_file(wav, &error), error);
        const RegionInfo& region = session.tracks()[0].regions[0];
        REQUIRE(region.source->clip->disk);
        CHECK(region.source->clip->samples.empty());  // nothing of it is kept in memory
        CHECK(region.length == frames);
        CHECK_FALSE(region.source->overview.minimum.empty());

        float value[2] = {};
        REQUIRE(toh::core::read_clip_frames(*region.source->clip, 2 * kPageFrames + 1234, 1, value) == 1);
        CHECK(value[0] == doctest::Approx(static_cast<float>((2 * kPageFrames + 1234) % 1000) / 2000.0f + 0.1f));

        folder = session.cache_folder();
        CHECK(std::filesystem::exists(folder));

        // play() loads the pages at the playhead before the engine needs them.
        session.play();
        std::vector<float> out(2 * 512);
        float loudest = 0.0f;
        for (int block = 0; block < 8; ++block) {
            session.engine().render(out.data(), 512);
            for (const float sample : out) {
                loudest = std::max(loudest, sample);
            }
        }
        CHECK(session.disk_misses() == 0);
        CHECK(loudest > 0.1f);

        // Jumping far ahead while playing: the new pages are loaded as well.
        session.seek_frames(3 * kPageFrames);
        for (int block = 0; block < 8; ++block) {
            session.engine().render(out.data(), 512);
        }
        CHECK(session.disk_misses() == 0);
    }
    CHECK_FALSE(std::filesystem::exists(folder));  // the cache is removed with the session
    std::filesystem::remove(wav);
}
