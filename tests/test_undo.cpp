// Tests for undo and redo.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/core/clip.h"

using toh::app::Session;
using toh::core::ClipData;

namespace {

std::shared_ptr<ClipData> stereo_clip(std::size_t frames, float value) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = 2;
    clip->sample_rate = 48000;
    clip->samples.assign(frames * 2, value);
    return clip;
}

}  // namespace

TEST_CASE("Undo and redo take edits back and forth") {
    Session session;
    CHECK_FALSE(session.can_undo());
    CHECK_FALSE(session.can_redo());

    REQUIRE(session.add_track("Guitar", stereo_clip(1000, 0.5f)));
    session.commit_edits();
    REQUIRE(session.can_undo());
    CHECK(session.undo_label() == "Add a track");

    // Many small moves before one commit (a drag) are one step.
    REQUIRE(session.split_region(0, 0, 400));
    session.commit_edits();
    CHECK(session.undo_label() == "Split or record");
    for (std::int64_t start = 410; start <= 2000; start += 10) {
        REQUIRE(session.move_region(0, 1, start));
    }
    session.commit_edits();
    CHECK(session.undo_label() == "Edit regions");
    CHECK(session.length_frames() == 2600);

    REQUIRE(session.undo());  // the drag
    CHECK(session.tracks()[0].regions[1].start == 400);
    CHECK(session.length_frames() == 1000);
    CHECK(session.can_redo());
    CHECK(session.redo_label() == "Edit regions");

    REQUIRE(session.undo());  // the split
    CHECK(session.tracks()[0].regions.size() == 1);
    REQUIRE(session.undo());  // the new track
    CHECK(session.tracks().empty());
    CHECK_FALSE(session.can_undo());
    CHECK_FALSE(session.undo());

    REQUIRE(session.redo());
    REQUIRE(session.redo());
    REQUIRE(session.redo());
    CHECK_FALSE(session.can_redo());
    REQUIRE(session.tracks().size() == 1);
    CHECK(session.tracks()[0].regions.size() == 2);
    CHECK(session.tracks()[0].regions[1].start == 2000);
    CHECK(session.length_frames() == 2600);

    // A new edit after an undo drops what could have been redone.
    REQUIRE(session.undo());
    REQUIRE(session.set_track_name(0, "Lead guitar"));
    session.commit_edits();
    CHECK(session.undo_label() == "Rename a track");
    CHECK_FALSE(session.can_redo());
}

TEST_CASE("Undo brings back removed tracks with their audio") {
    Session session;
    REQUIRE(session.add_track("Voice", stereo_clip(500, 0.25f)));
    session.add_empty_track("Empty");
    session.commit_edits();
    const auto source = session.tracks()[0].regions[0].source;

    REQUIRE(session.remove_track(0));
    session.commit_edits();
    CHECK(session.undo_label() == "Remove a track");
    REQUIRE(session.tracks().size() == 1);

    REQUIRE(session.undo());
    REQUIRE(session.tracks().size() == 2);
    CHECK(session.tracks()[0].name == "Voice");
    CHECK(session.tracks()[0].regions[0].source == source);
    CHECK(session.length_frames() == 500);
}

TEST_CASE("Mute, solo and the metronome are not undo steps; a new project clears the history") {
    Session session;
    REQUIRE(session.add_track("Drums", stereo_clip(500, 0.25f)));
    session.commit_edits();
    REQUIRE(session.set_track_mute(0, true));
    REQUIRE(session.set_track_solo(0, true));
    toh::app::MetronomeSettings metronome = session.metronome();
    metronome.enabled = true;
    REQUIRE(session.set_metronome(metronome));
    session.commit_edits();
    CHECK(session.undo_label() == "Add a track");

    REQUIRE(session.set_track_gain(0, 0.5f));
    session.commit_edits();
    CHECK(session.undo_label() == "Volume or pan");

    session.new_project();
    CHECK_FALSE(session.can_undo());
    CHECK_FALSE(session.can_redo());
}
