// Tests for cutting, copying, pasting and keeping a selected part of tracks.

#include <cstddef>
#include <cstdint>
#include <memory>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/core/clip.h"

using toh::app::Session;
using toh::core::ClipData;

namespace {

std::shared_ptr<ClipData> silent_clip(std::size_t frames) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = 2;
    clip->sample_rate = 48000;
    clip->samples.assign(frames * 2, 0.25f);
    return clip;
}

Session::Range range_of(std::size_t first, std::size_t last, std::int64_t begin, std::int64_t end) {
    Session::Range range;
    range.first_track = first;
    range.last_track = last;
    range.begin = begin;
    range.end = end;
    return range;
}

}  // namespace

TEST_CASE("Cutting the middle of a region leaves two parts and fills the clipboard") {
    Session session;
    REQUIRE(session.add_track("take", silent_clip(1000)));
    session.commit_edits();
    Session::Clipboard clipboard;
    REQUIRE(session.cut_range(range_of(0, 0, 300, 500), &clipboard));
    const auto& regions = session.tracks()[0].regions;
    REQUIRE(regions.size() == 2);
    CHECK(regions[0].start == 0);
    CHECK(regions[0].length == 300);
    CHECK(regions[1].start == 500);
    CHECK(regions[1].offset == 500);
    CHECK(regions[1].length == 500);
    REQUIRE(clipboard.tracks.size() == 1);
    REQUIRE(clipboard.tracks[0].size() == 1);
    CHECK(clipboard.tracks[0][0].start == 0);
    CHECK(clipboard.tracks[0][0].offset == 300);
    CHECK(clipboard.tracks[0][0].length == 200);
    CHECK(clipboard.length == 200);

    session.commit_edits();
    CHECK(session.undo_label() == "Cut");
    REQUIRE(session.undo());
    CHECK(session.tracks()[0].regions.size() == 1);
}

TEST_CASE("Copying and pasting puts the part at the given frame") {
    Session session;
    REQUIRE(session.add_track("take", silent_clip(1000)));
    const Session::Clipboard clipboard = session.copy_range(range_of(0, 0, 100, 400));
    CHECK(session.tracks()[0].regions.size() == 1);  // copying changes nothing
    REQUIRE(session.paste(clipboard, 0, 2000));
    const auto& regions = session.tracks()[0].regions;
    REQUIRE(regions.size() == 2);
    CHECK(regions[1].start == 2000);
    CHECK(regions[1].offset == 100);
    CHECK(regions[1].length == 300);
    CHECK(session.length_frames() >= 2300);
}

TEST_CASE("Pasting more tracks than there are adds tracks") {
    Session session;
    REQUIRE(session.add_track("a", silent_clip(1000)));
    REQUIRE(session.add_track("b", silent_clip(1000)));
    const Session::Clipboard clipboard = session.copy_range(range_of(0, 1, 0, 500));
    REQUIRE(clipboard.tracks.size() == 2);
    REQUIRE(session.paste(clipboard, 1, 0));
    REQUIRE(session.tracks().size() == 3);
    CHECK(session.tracks()[1].regions.size() == 2);
    CHECK(session.tracks()[2].regions.size() == 1);
}

TEST_CASE("Keeping only the selection removes the rest of those tracks") {
    Session session;
    REQUIRE(session.add_track("a", silent_clip(1000)));
    REQUIRE(session.add_track("b", silent_clip(1000)));
    REQUIRE(session.keep_range(range_of(0, 0, 200, 600)));
    const auto& kept = session.tracks()[0].regions;
    REQUIRE(kept.size() == 1);
    CHECK(kept[0].start == 200);
    CHECK(kept[0].offset == 200);
    CHECK(kept[0].length == 400);
    CHECK(session.tracks()[1].regions[0].length == 1000);  // another track stays whole
}
