// Tests for regions: placing pieces of clips on the timeline and editing them.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/core/clip.h"
#include "toh/core/engine.h"

using toh::app::GridStep;
using toh::app::Session;
using toh::core::ClipData;
using toh::core::Engine;
using toh::core::Region;

namespace {

constexpr std::uint32_t kRate = 48000;

// A clip in which the sample of frame n has the value n * scale on every channel.
std::shared_ptr<ClipData> ramp_clip(std::size_t frames, std::uint32_t channels, float scale) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = channels;
    clip->sample_rate = kRate;
    clip->samples.resize(frames * channels);
    for (std::size_t i = 0; i < frames; ++i) {
        for (std::size_t c = 0; c < channels; ++c) {
            clip->samples[i * channels + c] = static_cast<float>(i) * scale;
        }
    }
    return clip;
}

std::shared_ptr<ClipData> constant_clip(std::size_t frames, std::uint32_t channels, float value) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = channels;
    clip->sample_rate = kRate;
    clip->samples.assign(frames * channels, value);
    return clip;
}

Region make_region(std::shared_ptr<const ClipData> clip, std::int64_t start, std::int64_t offset,
                   std::int64_t length, std::int64_t fade_in = 0, std::int64_t fade_out = 0) {
    Region region;
    region.clip = std::move(clip);
    region.start = start;
    region.offset = offset;
    region.length = length;
    region.fade_in = fade_in;
    region.fade_out = fade_out;
    return region;
}

std::vector<float> render(Engine& engine, std::uint32_t frames) {
    std::vector<float> out(static_cast<std::size_t>(frames) * engine.channels(), 123.0f);
    engine.render(out.data(), frames);
    return out;
}

// Renders a stereo session without a sound device and returns the left channel.
std::vector<float> render_left(Session& session, std::uint32_t frames) {
    std::vector<float> out(static_cast<std::size_t>(frames) * 2);
    session.engine().render(out.data(), frames);
    std::vector<float> left(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        left[i] = out[2 * i];
    }
    return left;
}

}  // namespace

TEST_CASE("A region plays its part of the clip at its place on the timeline") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_track_regions(0, {make_region(ramp_clip(1000, 1, 1.0f), 500, 100, 200)}));
    REQUIRE(engine.play());

    const auto out = render(engine, 1000);
    for (std::size_t i = 0; i < 500; ++i) {
        CHECK(out[i] == 0.0f);
    }
    for (std::size_t k = 0; k < 200; ++k) {
        CHECK(out[500 + k] == doctest::Approx(static_cast<float>(100 + k)));
    }
    CHECK(out[700] == 0.0f);
    CHECK(engine.length_frames() == 700);
    CHECK_FALSE(engine.is_playing());
}

TEST_CASE("Regions on one track are mixed where they overlap") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_track_regions(0, {make_region(constant_clip(2000, 1, 0.25f), 0, 0, 1000),
                                         make_region(constant_clip(2000, 1, 0.5f), 600, 0, 1000)}));
    REQUIRE(engine.play());
    const auto out = render(engine, 2000);
    CHECK(out[300] == doctest::Approx(0.25f));
    CHECK(out[800] == doctest::Approx(0.75f));
    CHECK(out[1200] == doctest::Approx(0.5f));
    CHECK(out[1600] == 0.0f);
    CHECK(engine.length_frames() == 1600);
}

TEST_CASE("Region fades rise and fall smoothly") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_track_regions(
        0, {make_region(constant_clip(5000, 1, 1.0f), 1000, 0, 1000, 100, 200)}));
    REQUIRE(engine.play());
    const auto out = render(engine, 2500);

    CHECK(out[999] == 0.0f);
    CHECK(out[1000] > 0.0f);
    CHECK(out[1000] < 0.02f);
    for (std::size_t i = 1001; i <= 1100; ++i) {
        CHECK(out[i] >= out[i - 1]);
    }
    CHECK(out[1100] == doctest::Approx(1.0f));
    CHECK(out[1799] == doctest::Approx(1.0f));
    CHECK(out[1800] < 1.0f);
    CHECK(out[1999] > 0.0f);
    CHECK(out[1999] < 0.01f);
    CHECK(out[2000] == 0.0f);

    float largest_step = 0.0f;
    for (std::size_t i = 1000; i <= 2000; ++i) {
        largest_step = std::max(largest_step, std::fabs(out[i] - out[i - 1]));
    }
    CHECK(largest_step <= 0.0101f);

    // Fades longer than the region are shortened instead of misbehaving.
    Engine other(kRate, 1);
    REQUIRE(other.set_track_regions(
        0, {make_region(constant_clip(5000, 1, 1.0f), 500, 0, 100, 5000, 5000)}));
    REQUIRE(other.play());
    for (const float sample : render(other, 700)) {
        CHECK(sample >= 0.0f);
        CHECK(sample <= 1.0f);
    }
}

TEST_CASE("Regions can be changed during playback") {
    Engine engine(kRate, 1);
    const auto clip = constant_clip(100000, 1, 0.5f);
    REQUIRE(engine.set_track_regions(0, {make_region(clip, 0, 0, 100000)}));
    REQUIRE(engine.play());
    render(engine, 1000);

    REQUIRE(engine.set_track_regions(0, {make_region(clip, 1500, 0, 50000)}));
    const auto out = render(engine, 1000);
    CHECK(engine.is_playing());
    CHECK(engine.position_frames() == 2000);
    CHECK(out[100] == 0.0f);  // frame 1100: the region now starts later
    CHECK(out[900] > 0.0f);
    CHECK(engine.length_frames() == 51500);

    REQUIRE(engine.set_track_regions(0, {}));  // empty list: the track is emptied
    render(engine, 100);
    CHECK(engine.length_frames() == 0);
    CHECK_FALSE(engine.is_playing());
}

TEST_CASE("Regions that do not fit their clip are rejected") {
    Engine engine(kRate, 2);
    const auto clip = constant_clip(1000, 2, 0.5f);
    CHECK_FALSE(engine.set_track_regions(0, {make_region(clip, 0, 500, 501)}));
    CHECK_FALSE(engine.set_track_regions(0, {make_region(clip, 0, 0, 0)}));
    CHECK_FALSE(engine.set_track_regions(0, {make_region(clip, -1, 0, 100)}));
    CHECK_FALSE(engine.set_track_regions(0, {make_region(clip, 0, -1, 100)}));
    CHECK_FALSE(engine.set_track_regions(0, {make_region(nullptr, 0, 0, 100)}));
    CHECK_FALSE(engine.set_track_regions(0, {make_region(constant_clip(1000, 1, 0.5f), 0, 0, 100)}));
    CHECK_FALSE(engine.set_track_regions(Engine::kMaxTracks, {make_region(clip, 0, 0, 100)}));
    CHECK(engine.set_track_regions(0, {make_region(clip, 0, 500, 500)}));
}

TEST_CASE("Session moves and trims regions") {
    Session session;
    REQUIRE(session.add_track("ramp", ramp_clip(10000, 2, 0.001f)));
    REQUIRE(session.tracks()[0].regions.size() == 1);
    const auto& region = session.tracks()[0].regions[0];

    REQUIRE(session.move_region(0, 0, 5000));
    CHECK(region.start == 5000);
    CHECK(session.length_frames() == 15000);
    REQUIRE(session.move_region(0, 0, -300));  // not before the start of the timeline
    CHECK(region.start == 0);

    // Trimming the start keeps the audio where it is on the timeline.
    REQUIRE(session.trim_region_start(0, 0, 2000));
    CHECK(region.start == 2000);
    CHECK(region.offset == 2000);
    CHECK(region.length == 8000);
    session.play();
    auto left = render_left(session, 3000);
    CHECK(left[1999] == 0.0f);
    CHECK(left[2500] == doctest::Approx(2.5f));

    REQUIRE(session.trim_region_start(0, 0, -500));  // cannot reveal more than the source has
    CHECK(region.start == 0);
    CHECK(region.offset == 0);
    CHECK(region.length == 10000);
    REQUIRE(session.trim_region_start(0, 0, 999999));  // at least one frame stays
    CHECK(region.length == 1);
    REQUIRE(session.trim_region_start(0, 0, 0));

    REQUIRE(session.trim_region_end(0, 0, 6000));
    CHECK(region.length == 6000);
    CHECK(session.length_frames() == 6000);
    REQUIRE(session.trim_region_end(0, 0, 999999));  // not past the end of the source
    CHECK(region.length == 10000);
    REQUIRE(session.trim_region_end(0, 0, -5));
    CHECK(region.length == 1);
    REQUIRE(session.trim_region_end(0, 0, 10000));

    REQUIRE(session.set_region_fades(0, 0, 3000, 99999));
    CHECK(region.fade_in == 3000);
    CHECK(region.fade_out == 7000);
    REQUIRE(session.trim_region_end(0, 0, 4000));  // fades shrink with the region
    CHECK(region.fade_in == 3000);
    CHECK(region.fade_out == 1000);

    CHECK_FALSE(session.move_region(0, 1, 0));
    CHECK_FALSE(session.move_region(1, 0, 0));
    CHECK_FALSE(session.trim_region_start(3, 0, 0));
    CHECK_FALSE(session.set_region_fades(0, 7, 0, 0));
}

TEST_CASE("Session splits a region without changing the sound") {
    Session session;
    REQUIRE(session.add_track("ramp", ramp_clip(10000, 2, 0.001f)));
    REQUIRE(session.set_region_fades(0, 0, 500, 800));

    CHECK_FALSE(session.split_region(0, 0, 0));
    CHECK_FALSE(session.split_region(0, 0, 10000));
    REQUIRE(session.split_region(0, 0, 4000));

    const auto& regions = session.tracks()[0].regions;
    REQUIRE(regions.size() == 2);
    CHECK(regions[0].start == 0);
    CHECK(regions[0].length == 4000);
    CHECK(regions[0].fade_in == 500);
    CHECK(regions[0].fade_out == 0);
    CHECK(regions[1].start == 4000);
    CHECK(regions[1].offset == 4000);
    CHECK(regions[1].length == 6000);
    CHECK(regions[1].fade_in == 0);
    CHECK(regions[1].fade_out == 800);
    CHECK(session.length_frames() == 10000);

    session.play();
    const auto left = render_left(session, 5000);
    for (std::size_t i = 3990; i < 4010; ++i) {
        CHECK(left[i] == doctest::Approx(static_cast<float>(i) * 0.001f));
    }

    // The parts are independent: move the second one away and remove the first.
    REQUIRE(session.move_region(0, 1, 20000));
    CHECK(session.length_frames() == 26000);
    REQUIRE(session.remove_region(0, 0));
    REQUIRE(regions.size() == 1);
    CHECK(regions[0].start == 20000);
    CHECK_FALSE(session.remove_region(0, 1));
}

TEST_CASE("Session snaps positions to the grid") {
    Session session;  // 120 BPM in 4/4 at 48 kHz: one beat is 24000 frames
    CHECK(session.snap());
    CHECK(session.beat_frames() == doctest::Approx(24000.0));
    CHECK(session.bar_frames() == doctest::Approx(96000.0));

    CHECK(session.snap_frame(13000) == 24000);
    CHECK(session.snap_frame(11000) == 0);
    CHECK(session.snap_frame(-5000) == 0);

    session.set_grid_step(GridStep::HalfBeat);
    CHECK(session.snap_frame(13000) == 12000);
    session.set_grid_step(GridStep::QuarterBeat);
    CHECK(session.snap_frame(16000) == 18000);
    session.set_grid_step(GridStep::Bar);
    CHECK(session.snap_frame(50000) == 96000);

    // The grid follows the tempo and the time signature.
    auto settings = session.metronome();
    settings.bpm = 60.0;
    settings.beats_per_bar = 3;
    REQUIRE(session.set_metronome(settings));
    CHECK(session.bar_frames() == doctest::Approx(144000.0));
    session.set_grid_step(GridStep::Beat);
    CHECK(session.snap_frame(30000) == 48000);

    session.set_grid_step(GridStep::None);  // no grid: nothing snaps even with snapping on
    CHECK(session.snap_frame(30000) == 30000);
    session.set_grid_step(GridStep::Beat);
    CHECK(session.snap_frame(30000) == 48000);

    session.set_snap(false);
    CHECK(session.snap_frame(30000) == 30000);
}

TEST_CASE("Source gives exact values when zoomed in") {
    Session session;
    REQUIRE(session.add_track("ramp", ramp_clip(10000, 2, 0.001f)));
    const auto& source = *session.tracks()[0].regions[0].source;

    float low = 0.0f;
    float high = 0.0f;
    REQUIRE(source.range(1000, 1003, low, high));  // a few frames: read from the samples
    CHECK(low == doctest::Approx(1.0f));
    CHECK(high == doctest::Approx(1.003f));
    REQUIRE(source.range(0, 9999, low, high));  // everything: read from the overview
    CHECK(low == 0.0f);
    CHECK(high == doctest::Approx(9.999f));
    CHECK_FALSE(source.range(20000, 20010, low, high));
    CHECK_FALSE(source.range(10, 5, low, high));
}
