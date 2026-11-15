// Tests for the library: stock sounds, ready-made parts, and placing them on tracks.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>

#include "doctest.h"
#include "toh/app/library.h"
#include "toh/app/session.h"
#include "toh/core/stock_sounds.h"
#include "toh/core/stretch.h"

using toh::app::Session;

TEST_CASE("A project is saved in the background while the program goes on") {
    Session session;
    const std::size_t track = session.add_empty_track("Drums");
    REQUIRE(session.add_clip_region(track, 0, "Kick", toh::core::make_stock_sound(0, 48000, 2)));
    const auto folder = std::filesystem::temp_directory_path() / "toh_background_save";
    std::filesystem::create_directories(folder);
    const auto file = folder / "b.tohp";
    REQUIRE(session.save_project_in_background(file));
    CHECK_FALSE(session.save_project_in_background(file));  // one at a time
    session.add_marker(1000, "Made while saving");
    toh::app::Session::SaveResult result;
    for (int k = 0; k < 2000 && !result.done; ++k) {
        session.update();
        result = session.take_save_result();
        if (!result.done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    REQUIRE(result.done);
    CHECK(result.ok);
    CHECK_FALSE(session.saving());
    CHECK(session.project_file() == file);
    CHECK(session.modified());  // the marker came after the save began
    Session reopened;
    std::string error;
    REQUIRE_MESSAGE(reopened.open_project(file, &error), error);
    REQUIRE(reopened.tracks().size() == 1);
    CHECK(reopened.markers().empty());
    std::filesystem::remove_all(folder);
}

namespace {

// Upward zero crossings of the first channel, per second.
double crossings_per_second(const std::vector<float>& data, std::size_t channels, double rate) {
    int count = 0;
    const std::size_t frames = data.size() / channels;
    for (std::size_t i = 1; i < frames; ++i) {
        if (data[(i - 1) * channels] < 0.0f && data[i * channels] >= 0.0f) {
            ++count;
        }
    }
    return count / (static_cast<double>(frames) / rate);
}

}  // namespace

TEST_CASE("Stretching keeps the pitch, shifting the pitch keeps the length") {
    const auto tone = toh::core::make_sine_clip(441.0, 1.0, 48000, 2, 0.5f);
    const std::size_t frames = static_cast<std::size_t>(tone->frames());
    const std::vector<float> longer = toh::core::time_stretch(tone->samples.data(), frames, 2, 1.5);
    CHECK(longer.size() == static_cast<std::size_t>(std::llround(static_cast<double>(frames) * 1.5)) * 2);
    CHECK(crossings_per_second(longer, 2, 48000.0) == doctest::Approx(441.0).epsilon(0.03));
    const std::vector<float> higher = toh::core::pitch_shift(tone->samples.data(), frames, 2, 12.0);
    CHECK(higher.size() == tone->samples.size());
    CHECK(crossings_per_second(higher, 2, 48000.0) == doctest::Approx(882.0).epsilon(0.05));
}

TEST_CASE("The sound of a region is normalized, reversed and stretched, and can be undone") {
    Session session;
    REQUIRE(session.add_track("Tone", toh::core::make_sine_clip(441.0, 1.0, 48000, 2, 0.25f)));
    session.commit_edits();
    REQUIRE(session.edit_region_sound(0, 0, toh::app::SoundEdit::Normalize));
    session.commit_edits();
    CHECK(session.undo_label() == "Normalize");
    std::vector<float> data(2 * 48000);
    const auto& region = session.tracks()[0].regions[0];
    toh::core::read_clip_frames(*region.source->clip, 0, 48000, data.data());
    float peak = 0.0f;
    for (const float value : data) {
        peak = std::max(peak, std::fabs(value));
    }
    CHECK(peak == doctest::Approx(0.98f).epsilon(0.01));
    REQUIRE(session.edit_region_sound(0, 0, toh::app::SoundEdit::Stretch, 2.0));
    CHECK(session.tracks()[0].regions[0].length == 96000);
    CHECK_FALSE(session.edit_region_sound(0, 0, toh::app::SoundEdit::Stretch, 9.0));
    session.commit_edits();
    REQUIRE(session.undo());
    CHECK(session.tracks()[0].regions[0].length == 48000);
}

namespace {

// The loudest sample of the last tenth of a second, after a second has played.
float settled_peak(Session& session) {
    session.seek_frames(0);
    session.play();
    std::vector<float> block(2 * 4800);
    float peak = 0.0f;
    for (int k = 0; k < 10; ++k) {
        session.engine().render(block.data(), 4800);
    }
    for (const float value : block) {
        peak = std::max(peak, std::fabs(value));
    }
    session.stop();
    return peak;
}

}  // namespace

TEST_CASE("Tracks in a group sum on its bus, with its fader and mute") {
    Session session;
    REQUIRE(session.add_track("A", toh::core::make_sine_clip(441.0, 3.0, 48000, 2, 0.25f)));
    REQUIRE(session.add_track("B", toh::core::make_sine_clip(441.0, 3.0, 48000, 2, 0.25f)));
    REQUIRE(session.add_track("C", toh::core::make_sine_clip(441.0, 3.0, 48000, 2, 0.25f)));
    session.commit_edits();
    const float all = settled_peak(session);
    REQUIRE(all > 0.5f);

    const std::size_t group = session.add_group("  Backing  ", {0, 1});
    REQUIRE(group == 0);
    session.commit_edits();
    CHECK(session.undo_label() == "Make a group");
    CHECK(session.groups()[0].name == "Backing");
    CHECK(session.group_tracks(0) == std::vector<std::size_t>{0, 1});
    CHECK(settled_peak(session) == doctest::Approx(all).epsilon(0.01));

    REQUIRE(session.set_group_gain(0, 0.5f));
    CHECK(settled_peak(session) == doctest::Approx(all * 2.0f / 3.0f).epsilon(0.02));
    REQUIRE(session.set_group_mute(0, true));
    CHECK(settled_peak(session) == doctest::Approx(all / 3.0f).epsilon(0.02));
    REQUIRE(session.set_group_mute(0, false));
    session.commit_edits();

    // Saved and opened again.
    const auto folder = std::filesystem::temp_directory_path() / "toh_groups";
    std::filesystem::create_directories(folder);
    const auto file = folder / "g.tohp";
    std::string error;
    REQUIRE(session.set_group_collapsed(0, true));
    REQUIRE_MESSAGE(session.save_project(file, &error), error);
    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(file, &error), error);
    REQUIRE(reopened.groups().size() == 1);
    CHECK(reopened.groups()[0].name == "Backing");
    CHECK(reopened.groups()[0].gain == doctest::Approx(0.5f));
    CHECK(reopened.groups()[0].collapsed);
    CHECK(reopened.tracks()[1].group == 0);
    CHECK(reopened.tracks()[2].group == -1);
    CHECK(settled_peak(reopened) == doctest::Approx(all * 2.0f / 3.0f).epsilon(0.02));
    std::filesystem::remove_all(folder);

    // Removing the group sends its tracks to the master again; undo brings it back.
    REQUIRE(session.remove_group(0));
    CHECK(session.tracks()[0].group == -1);
    session.commit_edits();
    CHECK(settled_peak(session) == doctest::Approx(all).epsilon(0.01));
    REQUIRE(session.undo());
    REQUIRE(session.groups().size() == 1);
    CHECK(session.tracks()[1].group == 0);
    CHECK(settled_peak(session) == doctest::Approx(all * 2.0f / 3.0f).epsilon(0.02));
    CHECK_FALSE(session.set_track_group(2, 3));
    REQUIRE(session.set_track_group(2, 0));
    CHECK(session.group_tracks(0).size() == 3);
}

TEST_CASE("Every stock sound is made, is not silent and is always the same") {
    REQUIRE(toh::core::stock_sound_count() >= 20);
    for (std::size_t k = 0; k < toh::core::stock_sound_count(); ++k) {
        const auto info = toh::core::stock_sound_info(k);
        CAPTURE(std::string(info.name));
        const auto clip = toh::core::make_stock_sound(k, 48000, 2);
        REQUIRE(clip);
        REQUIRE(clip->frames() > 1000);
        float peak = 0.0f;
        for (const float value : clip->samples) {
            REQUIRE(std::isfinite(value));
            peak = std::max(peak, std::fabs(value));
        }
        CHECK(peak == doctest::Approx(0.79f).epsilon(0.01));
        const auto again = toh::core::make_stock_sound(k, 48000, 2);
        CHECK(again->samples == clip->samples);
    }
    CHECK_FALSE(toh::core::make_stock_sound(toh::core::stock_sound_count(), 48000, 2));
}

TEST_CASE("Stock sounds and parts are put onto tracks") {
    Session session;
    const std::size_t audio = session.add_empty_track("Drums");
    REQUIRE(session.add_clip_region(audio, 48000, "Kick", toh::core::make_stock_sound(0, 48000, 2)));
    REQUIRE(session.add_clip_region(audio, 0, "Snare", toh::core::make_stock_sound(3, 48000, 2)));
    REQUIRE(session.tracks()[audio].regions.size() == 2);
    CHECK(session.tracks()[audio].regions[0].start == 0);  // kept in order of time
    CHECK(session.tracks()[audio].regions[1].start == 48000);

    const toh::app::PartInfo& part = toh::app::library_parts().front();
    std::int64_t length = 0;
    const auto notes = toh::app::part_notes(session, part, 96000, &length);
    REQUIRE_FALSE(notes.empty());
    CHECK(length == static_cast<std::int64_t>(std::llround(part.bars * 4.0 * session.beat_frames())));
    for (const auto& note : notes) {
        CHECK(note.start >= 0);
        CHECK(note.start + note.length <= length);
    }
    CHECK_FALSE(session.add_note_region(audio, 0, length, "Beat", notes));  // not on recorded sound
    const std::size_t keys = session.add_instrument_track("Keys", toh::core::Instrument::Keys, 0, 48000, 0);
    REQUIRE(session.add_note_region(keys, 96000, length, "Beat", notes));
    REQUIRE(session.tracks()[keys].regions.size() == 2);
    const auto& placed = session.tracks()[keys].regions[1];
    CHECK(placed.start == 96000);
    CHECK(placed.source->clip->notes->notes.size() == notes.size());
    CHECK_FALSE(session.add_clip_region(keys, 0, "Kick", toh::core::make_stock_sound(0, 48000, 2)));

    for (const toh::app::PartInfo& each : toh::app::library_parts()) {
        CAPTURE(std::string(each.name));
        CHECK_FALSE(each.notes.empty());
        for (const auto& note : each.notes) {
            CHECK(note.beat + note.length <= each.bars * 4.0 + 1e-9);
        }
    }
}
