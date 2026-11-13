#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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

// Renders without a sound device and returns the left sample of the last frame.
float last_left(Session& session, std::uint32_t frames) {
    std::vector<float> out(static_cast<std::size_t>(frames) * 2);
    session.engine().render(out.data(), frames);
    return out[out.size() - 2];
}

}  // namespace

TEST_CASE("Overview stores the lowest and highest value of every bucket") {
    ClipData clip;
    clip.channels = 1;
    clip.sample_rate = 48000;
    clip.samples = {0.0f, 0.5f, -1.0f, 0.25f, 0.75f};

    const auto overview = toh::app::make_overview(clip, 2);
    REQUIRE(overview.minimum.size() == 3);
    CHECK(overview.minimum[0] == 0.0f);
    CHECK(overview.maximum[0] == 0.5f);
    CHECK(overview.minimum[1] == -1.0f);
    CHECK(overview.maximum[1] == 0.25f);
    CHECK(overview.minimum[2] == 0.75f);
    CHECK(overview.maximum[2] == 0.75f);

    float low = 0.0f;
    float high = 0.0f;
    REQUIRE(overview.range(0, 4, low, high));
    CHECK(low == -1.0f);
    CHECK(high == 0.75f);
    REQUIRE(overview.range(4, 100, low, high));  // past the end: clamped
    CHECK(low == 0.75f);
    CHECK_FALSE(overview.range(6, 9, low, high));
    CHECK_FALSE(overview.range(3, 2, low, high));
}

TEST_CASE("Session keeps its track list and the engine in step") {
    Session session;  // no sound device is opened in this test
    std::string error;

    REQUIRE(session.add_track("quiet", stereo_clip(100000, 0.25f), &error));
    REQUIRE(session.add_track("loud", stereo_clip(50000, 0.5f), &error));
    CHECK(session.tracks().size() == 2);
    CHECK(session.length_frames() == 100000);
    CHECK(session.tracks()[1].name == "loud");
    REQUIRE(session.tracks()[0].regions.size() == 1);
    CHECK_FALSE(session.tracks()[0].regions[0].source->overview.minimum.empty());
    CHECK(session.tracks()[0].regions[0].length == 100000);

    session.play();
    CHECK(last_left(session, 4800) == doctest::Approx(0.75f));

    REQUIRE(session.set_track_gain(1, 0.5f));
    CHECK(session.tracks()[1].gain == 0.5f);
    CHECK(last_left(session, 4800) == doctest::Approx(0.5f).epsilon(0.001));
    CHECK_FALSE(session.set_track_gain(2, 1.0f));
    CHECK_FALSE(session.set_track_gain(0, -1.0f));
    CHECK(session.tracks()[0].gain == 1.0f);

    // Removing the first track moves the second one up, together with its settings.
    REQUIRE(session.remove_track(0));
    session.update();
    CHECK(session.tracks().size() == 1);
    CHECK(session.length_frames() == 50000);
    session.play();
    CHECK(last_left(session, 4800) == doctest::Approx(0.25f));
    CHECK(session.position_frames() == 3 * 4800);  // changing tracks does not stop playback

    REQUIRE(session.remove_track(0));
    CHECK_FALSE(session.remove_track(0));
    session.play();
    CHECK(last_left(session, 480) == 0.0f);
    CHECK(session.length_frames() == 0);
}

TEST_CASE("Session rejects clips in the wrong format") {
    Session session;
    auto mono = std::make_shared<ClipData>();
    mono->channels = 1;
    mono->sample_rate = 48000;
    mono->samples.assign(100, 0.5f);

    std::string error;
    CHECK_FALSE(session.add_track("mono", mono, &error));
    CHECK_FALSE(error.empty());
    CHECK(session.tracks().empty());
}

TEST_CASE("Session works without a sound device") {
    Session session;
    // Far more control messages than the engine's queue holds: update() must keep it drained.
    for (int i = 0; i < 5000; ++i) {
        REQUIRE(session.set_master_gain(0.5f));
        session.update();
    }
    CHECK(session.master_gain() == 0.5f);
}

TEST_CASE("Session adds empty tracks that can be recorded onto") {
    Session session;
    const std::size_t first = session.add_empty_track("Guitar");
    const std::size_t second = session.add_empty_track("Voice");
    CHECK(first == 0);
    CHECK(second == 1);
    REQUIRE(session.tracks().size() == 2);
    CHECK(session.tracks()[1].name == "Voice");
    CHECK(session.tracks()[1].regions.empty());
    CHECK(session.length_frames() == 0);

    // Record onto the second track: the take lands there, not on a new track.
    session.set_record_offset_frames(0);
    REQUIRE(session.start_recording(second));
    session.update();
    std::vector<float> in(2 * 4800, 0.25f);
    std::vector<float> out(in.size());
    session.engine().render(in.data(), out.data(), 4800);
    session.update();
    session.stop_recording();
    session.update();
    session.engine().render(in.data(), out.data(), 1000);
    session.update();
    CHECK(session.tracks().size() == 2);
    CHECK(session.tracks()[0].regions.empty());
    REQUIRE(session.tracks()[1].regions.size() == 1);
    CHECK(session.tracks()[1].regions[0].start == 0);

    // The limit is respected.
    while (session.tracks().size() < toh::core::Engine::kMaxTracks) {
        session.add_empty_track("more");
    }
    CHECK(session.add_empty_track("one too many") == session.tracks().size());
}

TEST_CASE("Session can play only the selected tracks") {
    Session session;
    session.add_empty_track("one");
    session.add_empty_track("two");
    session.add_empty_track("three");
    const auto audible = [&](bool a, bool b, bool c) {
        return session.track_audible(0) == a && session.track_audible(1) == b &&
               session.track_audible(2) == c;
    };
    CHECK(audible(true, true, true));

    REQUIRE(session.set_track_selected(1, true));
    CHECK(audible(true, true, true));  // the option is off
    session.set_play_selected_only(true);
    CHECK(session.play_selected_only());
    CHECK(audible(false, true, false));
    REQUIRE(session.set_track_selected(2, true));
    CHECK(audible(false, true, true));

    session.select_only_track(0);
    CHECK(session.tracks()[0].selected);
    CHECK_FALSE(session.tracks()[1].selected);
    CHECK(audible(true, false, false));
    REQUIRE(session.set_track_mute(0, true));
    CHECK(audible(false, false, false));
    REQUIRE(session.set_track_mute(0, false));

    session.clear_track_selection();
    CHECK(audible(true, true, true));  // nothing selected: every track plays

    session.select_only_track(2);
    REQUIRE(session.remove_track(2));
    CHECK(session.track_audible(0));
    CHECK(session.track_audible(1));

    REQUIRE(session.set_track_solo(1, true));
    CHECK_FALSE(session.track_audible(0));
    CHECK(session.track_audible(1));
    CHECK_FALSE(session.set_track_selected(7, true));
}

TEST_CASE("Session renames tracks and gives them pictures") {
    Session session;
    session.add_empty_track("Track 1");
    REQUIRE(session.set_track_name(0, "  \xD0\x93\xD0\xB8\xD1\x82\xD0\xB0\xD1\x80\xD0\xB0 solo \t"));
    CHECK(session.tracks()[0].name == "\xD0\x93\xD0\xB8\xD1\x82\xD0\xB0\xD1\x80\xD0\xB0 solo");
    CHECK_FALSE(session.set_track_name(0, "   "));
    CHECK(session.tracks()[0].name == "\xD0\x93\xD0\xB8\xD1\x82\xD0\xB0\xD1\x80\xD0\xB0 solo");
    CHECK_FALSE(session.set_track_name(1, "No such track"));

    CHECK(session.tracks()[0].icon.empty());
    REQUIRE(session.set_track_icon(0, "guitar"));
    CHECK(session.tracks()[0].icon == "guitar");
    REQUIRE(session.set_track_icon(0, "file:C:/Pictures/\xD0\xB3\xD0\xB8\xD1\x82\xD0\xB0\xD1\x80\xD0\xB0.png"));
    CHECK(session.tracks()[0].icon.rfind("file:", 0) == 0);
    REQUIRE(session.set_track_icon(0, ""));
    CHECK(session.tracks()[0].icon.empty());
    CHECK_FALSE(session.set_track_icon(5, "drums"));
}
