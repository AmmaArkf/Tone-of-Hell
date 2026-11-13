// Tests for saving and opening projects.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "miniz.h"
#include "toh/app/session.h"
#include "toh/core/clip.h"

using toh::app::RegionInfo;
using toh::app::Session;
using toh::app::TrackInfo;
using toh::core::ClipData;

namespace {

std::shared_ptr<ClipData> counting_clip(std::size_t frames, float scale, std::uint32_t rate = 48000) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = 2;
    clip->sample_rate = rate;
    clip->samples.resize(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        clip->samples[i * 2] = static_cast<float>(i) * scale;
        clip->samples[i * 2 + 1] = -static_cast<float>(i) * scale;
    }
    return clip;
}

float sample_at(const RegionInfo& region, std::int64_t frame) {
    float value[2] = {};
    toh::core::read_clip_frames(*region.source->clip, region.offset + frame, 1, value);
    return value[0];
}

// A fresh, empty folder for one test.
std::filesystem::path test_folder(const char* name) {
    const std::filesystem::path folder = std::filesystem::temp_directory_path() / name;
    std::filesystem::remove_all(folder);
    std::filesystem::create_directories(folder);
    return folder;
}

// Writes a zip file with one "project.txt" entry.
void write_zip(const std::filesystem::path& path, const std::string& text) {
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    REQUIRE(mz_zip_writer_init_file(&zip, path.string().c_str(), 0));
    REQUIRE(mz_zip_writer_add_mem(&zip, "project.txt", text.data(), text.size(), MZ_DEFAULT_LEVEL));
    REQUIRE(mz_zip_writer_finalize_archive(&zip));
    mz_zip_writer_end(&zip);
}

}  // namespace

TEST_CASE("A saved project opens again exactly as it was") {
    const std::filesystem::path folder = test_folder("toh_test_project");
    const std::filesystem::path file =
        folder / std::filesystem::u8path("\xD0\x9F\xD0\xB5\xD1\x81\xD0\xBD\xD1\x8F.tohp");

    {
        Session session;
        CHECK_FALSE(session.modified());
        REQUIRE(session.add_track("Guitar", counting_clip(3000, 0.0001f)));
        REQUIRE(session.add_track("Voice", counting_clip(2000, 0.0002f)));
        session.add_empty_track("Empty");
        CHECK(session.modified());
        REQUIRE(session.split_region(0, 0, 1000));
        REQUIRE(session.move_region(0, 1, 5000));
        REQUIRE(session.set_region_fades(0, 0, 100, 200));
        REQUIRE(session.set_track_icon(0, "guitar"));
        {
            std::ofstream picture(folder / "picture.png", std::ios::binary);
            picture << "not really a PNG, but the bytes must come back";
        }
        REQUIRE(session.set_track_icon(1, "file:" + (folder / "picture.png").u8string()));
        REQUIRE(session.set_track_gain(1, 0.5f));
        REQUIRE(session.set_track_pan(1, -0.25f));
        REQUIRE(session.set_track_mute(2, true));
        toh::app::MetronomeSettings metronome = session.metronome();
        metronome.bpm = 97.5;
        metronome.beats_per_bar = 3;
        REQUIRE(session.set_metronome(metronome));
        session.set_loop(true);

        std::string error;
        REQUIRE_MESSAGE(session.save_project(file, &error), error);
        CHECK(session.project_file() == file);
        CHECK_FALSE(session.modified());
        CHECK(std::filesystem::is_regular_file(file));
        std::filesystem::remove(folder / "picture.png");  // the project has its own copy
        // One file only: no folders next to it.
        std::size_t entries = 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder)) {
            (void)entry;
            ++entries;
        }
        CHECK(entries == 1);

        REQUIRE(session.set_track_gain(0, 0.75f));
        CHECK(session.modified());
        REQUIRE(session.set_track_gain(0, 1.0f));
        CHECK_FALSE(session.modified());  // back as saved
    }

    Session opened;
    opened.add_empty_track("Something that goes away");
    opened.add_empty_track("And this too");
    opened.add_empty_track("And this");
    opened.add_empty_track("And this one");
    std::string error;
    REQUIRE_MESSAGE(opened.open_project(file, &error), error);
    CHECK(opened.project_file() == file);
    CHECK_FALSE(opened.modified());
    const std::vector<TrackInfo>& tracks = opened.tracks();
    REQUIRE(tracks.size() == 3);
    CHECK(tracks[0].name == "Guitar");
    CHECK(tracks[0].icon == "guitar");
    CHECK(tracks[1].name == "Voice");
    REQUIRE(tracks[1].icon.rfind("file:", 0) == 0);
    {
        std::ifstream picture(std::filesystem::u8path(tracks[1].icon.substr(5)), std::ios::binary);
        REQUIRE(picture);
        std::string bytes((std::istreambuf_iterator<char>(picture)), std::istreambuf_iterator<char>());
        CHECK(bytes == "not really a PNG, but the bytes must come back");
    }
    CHECK(tracks[1].gain == doctest::Approx(0.5f));
    CHECK(tracks[1].pan == doctest::Approx(-0.25f));
    CHECK(tracks[2].mute);
    CHECK(tracks[2].regions.empty());
    CHECK(opened.metronome().bpm == doctest::Approx(97.5));
    CHECK(opened.metronome().beats_per_bar == 3);
    CHECK(opened.loop());

    REQUIRE(tracks[0].regions.size() == 2);
    const RegionInfo& first = tracks[0].regions[0];
    const RegionInfo& second = tracks[0].regions[1];
    CHECK(first.start == 0);
    CHECK(first.length == 1000);
    CHECK(first.fade_in == 100);
    CHECK(first.fade_out == 200);
    CHECK(second.start == 5000);
    CHECK(second.offset == 1000);
    CHECK(second.length == 2000);
    CHECK(first.source == second.source);  // both parts still share one piece of audio
    CHECK(sample_at(second, 10) == doctest::Approx(1010 * 0.0001f));
    CHECK(sample_at(tracks[1].regions[0], 1999) == doctest::Approx(1999 * 0.0002f));
    CHECK(opened.length_frames() == 7000);

    // The opened project saves again, also after changes.
    REQUIRE(opened.remove_track(1));
    CHECK(opened.modified());
    REQUIRE_MESSAGE(opened.save_project(file, &error), error);
    CHECK_FALSE(opened.modified());

    Session again;
    REQUIRE_MESSAGE(again.open_project(file, &error), error);
    REQUIRE(again.tracks().size() == 2);
    CHECK(sample_at(again.tracks()[0].regions[1], 10) == doctest::Approx(1010 * 0.0001f));

    opened.new_project();
    CHECK(opened.tracks().empty());
    CHECK(opened.project_file().empty());
    CHECK(opened.length_frames() == 0);
    CHECK_FALSE(opened.modified());
}

TEST_CASE("Opening refuses files that are not projects or do not fit") {
    const std::filesystem::path folder = test_folder("toh_test_project_bad");
    Session session;
    session.add_empty_track("Stays");
    std::string error;

    CHECK_FALSE(session.open_project(folder / "missing.tohp", &error));
    CHECK_FALSE(error.empty());

    {
        std::ofstream out(folder / "other.tohp");
        out << "something else\n";
    }
    CHECK_FALSE(session.open_project(folder / "other.tohp", &error));

    {
        toh::audio::DeviceConfig config;
        config.sample_rate = 44100;
        Session other(config);
        REQUIRE(other.add_track("At 44.1 kHz", counting_clip(100, 0.01f, 44100)));
        REQUIRE(other.save_project(folder / "rate.tohp", &error));
    }
    error.clear();
    CHECK_FALSE(session.open_project(folder / "rate.tohp", &error));
    CHECK(error.find("44100") != std::string::npos);

    write_zip(folder / "escape.tohp",
              "Tone of Hell project\nversion=1\nsample_rate=48000\nchannels=2\n"
              "[source]\nfile=../secret.f32\nname=x\n");
    CHECK_FALSE(session.open_project(folder / "escape.tohp", &error));

    write_zip(folder / "lost.tohp",
              "Tone of Hell project\nversion=1\nsample_rate=48000\nchannels=2\n"
              "[source]\nfile=audio-1.f32\nname=x\n");
    error.clear();
    CHECK_FALSE(session.open_project(folder / "lost.tohp", &error));
    CHECK_FALSE(error.empty());

    REQUIRE(session.tracks().size() == 1);  // nothing changed
    CHECK(session.tracks()[0].name == "Stays");
}
