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
