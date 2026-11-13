// Tests for reverse playback and the metronome.

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

using toh::core::ClipData;
using toh::core::Engine;

namespace {

constexpr std::uint32_t kRate = 48000;

// A mono clip whose sample number n has the value n: the output shows which frame was played.
std::shared_ptr<ClipData> index_clip(std::size_t frames) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = 1;
    clip->sample_rate = kRate;
    clip->samples.resize(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        clip->samples[i] = static_cast<float>(i);
    }
    return clip;
}

std::shared_ptr<ClipData> constant_clip(std::size_t frames, float value) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = 1;
    clip->sample_rate = kRate;
    clip->samples.assign(frames, value);
    return clip;
}

std::vector<float> render(Engine& engine, std::uint32_t frames) {
    std::vector<float> out(static_cast<std::size_t>(frames) * engine.channels(), 123.0f);
    engine.render(out.data(), frames);
    return out;
}

float peak(const std::vector<float>& samples, std::size_t first, std::size_t last) {
    float largest = 0.0f;
    for (std::size_t i = first; i < last; ++i) {
        largest = std::max(largest, std::fabs(samples[i]));
    }
    return largest;
}

int zero_crossings(const std::vector<float>& samples, std::size_t first, std::size_t last) {
    int count = 0;
    for (std::size_t i = first + 1; i < last; ++i) {
        if ((samples[i - 1] < 0.0f) != (samples[i] < 0.0f)) {
            ++count;
        }
    }
    return count;
}

}  // namespace

TEST_CASE("Reverse plays the frames in descending order and stops at the start") {
    Engine engine(kRate, 1);
    const std::uint32_t fade = engine.fade_frames();
    REQUIRE(fade < 350);
    REQUIRE(engine.set_clip(index_clip(1000)));
    REQUIRE(engine.seek(600));
    REQUIRE(engine.set_reverse(true));
    REQUIRE(engine.play());

    auto out = render(engine, 400);
    for (std::size_t i = fade + 1; i < out.size(); ++i) {
        CHECK(out[i] == doctest::Approx(static_cast<float>(599 - static_cast<int>(i))));
    }
    CHECK(engine.is_playing());
    CHECK(engine.position_frames() == 200);

    out = render(engine, 400);
    CHECK(out[198] == doctest::Approx(1.0f));  // frame 1, then frame 0, then silence
    CHECK(out[200] == 0.0f);
    CHECK(out.back() == 0.0f);
    CHECK_FALSE(engine.is_playing());
    CHECK(engine.position_frames() == 0);
}

TEST_CASE("Reverse started at the beginning plays from the end") {
    Engine engine(kRate, 1);
    const std::uint32_t fade = engine.fade_frames();
    REQUIRE(engine.set_clip(index_clip(1000)));
    REQUIRE(engine.set_reverse(true));
    REQUIRE(engine.play());

    const auto out = render(engine, 400);
    for (std::size_t i = fade + 1; i < out.size(); ++i) {
        CHECK(out[i] == doctest::Approx(static_cast<float>(999 - static_cast<int>(i))));
    }
}

TEST_CASE("Reverse stays inside the loop region") {
    Engine engine(kRate, 1);
    const std::uint32_t fade = engine.fade_frames();
    REQUIRE(engine.set_clip(index_clip(1000)));
    REQUIRE(engine.set_loop(true, 100, 200));
    REQUIRE(engine.seek(150));
    REQUIRE(engine.set_reverse(true));
    REQUIRE(engine.play());

    const auto out = render(engine, 2000);
    for (std::size_t i = fade + 1; i < out.size(); ++i) {
        CHECK(out[i] >= 100.0f - 0.01f);
        CHECK(out[i] < 200.0f);
    }
    CHECK(out[fade + 60] > out[fade + 61]);  // still descending
    CHECK(engine.is_playing());
}

TEST_CASE("Changing direction during playback does not click") {
    Engine engine(kRate, 1);
    const std::uint32_t fade = engine.fade_frames();
    REQUIRE(engine.set_clip(constant_clip(200000, 1.0f)));
    REQUIRE(engine.play());

    std::vector<float> all = render(engine, 5000);
    REQUIRE(engine.set_reverse(true));
    auto out = render(engine, 2000);
    all.insert(all.end(), out.begin(), out.end());
    const std::int64_t turned_at = engine.position_frames();
    CHECK(turned_at < 5000 + fade);  // it turned around right after the fade-out

    out = render(engine, 1000);
    all.insert(all.end(), out.begin(), out.end());
    CHECK(engine.position_frames() == turned_at - 1000);
    CHECK(engine.is_playing());

    REQUIRE(engine.set_reverse(false));
    out = render(engine, 2000);
    all.insert(all.end(), out.begin(), out.end());
    out = render(engine, 100);
    CHECK(engine.position_frames() > turned_at - 1000 - static_cast<std::int64_t>(fade));

    float largest_step = 0.0f;
    float previous = 0.0f;
    for (const float sample : all) {
        largest_step = std::max(largest_step, std::fabs(sample - previous));
        previous = sample;
    }
    CHECK(largest_step <= 1.5f / static_cast<float>(fade));
    CHECK(all.back() == doctest::Approx(1.0f));
}

TEST_CASE("Metronome clicks on every beat, also without any clip") {
    Engine engine(kRate, 1);

    // Nothing loaded and no metronome: play does nothing.
    REQUIRE(engine.play());
    render(engine, 100);
    CHECK_FALSE(engine.is_playing());

    REQUIRE(engine.set_metronome(true, 120.0, 4, 4, 1.0f));  // one beat = 24000 frames
    REQUIRE(engine.play());
    const auto out = render(engine, 100000);
    CHECK(engine.is_playing());
    CHECK(engine.position_frames() == 100000);

    CHECK(peak(out, 24000, 24500) > 0.3f);
    CHECK(peak(out, 48000, 48500) > 0.3f);
    CHECK(peak(out, 72000, 72500) > 0.3f);
    CHECK(peak(out, 96000, 96500) > 0.3f);
    CHECK(peak(out, 2000, 24000) == 0.0f);  // silence between the clicks
    CHECK(peak(out, 26000, 48000) == 0.0f);
    CHECK(peak(out, 0, 100000) <= 1.0f);

    // The first beat of a bar (frame 96000 is beat 5 = bar 2, beat 1) sounds higher.
    CHECK(zero_crossings(out, 96000, 96240) > zero_crossings(out, 24000, 24240) * 3 / 2);

    // Turning it off ends the endless playback of an empty project.
    REQUIRE(engine.set_metronome(false));
    const auto after = render(engine, 4000);
    CHECK_FALSE(engine.is_playing());
    CHECK(after.back() == 0.0f);
}

TEST_CASE("Metronome follows the tempo, the time signature and the volume") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_metronome(true, 90.0, 3, 4, 0.5f));  // one beat = 32000 frames
    REQUIRE(engine.play());
    auto out = render(engine, 100000);
    CHECK(peak(out, 32000, 32500) > 0.15f);
    CHECK(peak(out, 32000, 32500) <= 0.5f);
    CHECK(peak(out, 2000, 32000) == 0.0f);
    // In 3/4 the fourth beat (frame 96000) starts a new bar.
    CHECK(zero_crossings(out, 96000, 96240) > zero_crossings(out, 32000, 32240) * 3 / 2);

    // An eighth-note beat unit halves the distance between clicks.
    Engine eighths(kRate, 1);
    REQUIRE(eighths.set_metronome(true, 90.0, 6, 8, 1.0f));  // one beat = 16000 frames
    REQUIRE(eighths.play());
    out = render(eighths, 40000);
    CHECK(peak(out, 16000, 16500) > 0.3f);
    CHECK(peak(out, 32000, 32500) > 0.3f);
    CHECK(peak(out, 18000, 32000) == 0.0f);
}

TEST_CASE("Metronome is mixed on top of the tracks and ignores the master fader") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_clip(constant_clip(100000, 0.25f)));
    REQUIRE(engine.set_gain(0.0f));  // master all the way down
    REQUIRE(engine.set_metronome(true, 120.0, 4, 4, 1.0f));
    REQUIRE(engine.play());
    const auto out = render(engine, 50000);
    CHECK(peak(out, 24000, 24500) > 0.3f);   // the click is still there
    CHECK(peak(out, 26000, 48000) == 0.0f);  // the track is not
}

TEST_CASE("Metronome rejects values out of range") {
    Engine engine(kRate, 2);
    CHECK_FALSE(engine.set_metronome(true, 10.0));
    CHECK_FALSE(engine.set_metronome(true, 500.0));
    CHECK_FALSE(engine.set_metronome(true, std::nan("")));
    CHECK_FALSE(engine.set_metronome(true, 120.0, 0));
    CHECK_FALSE(engine.set_metronome(true, 120.0, 4, 3));
    CHECK_FALSE(engine.set_metronome(true, 120.0, 4, 4, 3.0f));
    CHECK(engine.set_metronome(true, 120.0, 7, 8, 2.0f));

    toh::app::Session session;
    toh::app::MetronomeSettings settings = session.metronome();
    settings.bpm = 1000.0;
    CHECK_FALSE(session.set_metronome(settings));
    CHECK(session.metronome().bpm == 120.0);
    settings.bpm = 140.0;
    settings.enabled = true;
    CHECK(session.set_metronome(settings));
    CHECK(session.metronome().bpm == 140.0);
    CHECK(session.metronome().enabled);

    session.set_reverse(true);
    CHECK(session.reverse());
}

TEST_CASE("The reverse button can leave playback stopped") {
    toh::app::Session session;
    auto clip = std::make_shared<ClipData>();
    clip->channels = 2;
    clip->sample_rate = kRate;
    clip->samples.assign(2 * 100000, 0.5f);
    REQUIRE(session.add_track("track", clip));
    std::vector<float> out(2 * 1000);

    session.set_reverse_starts_playback(false);
    session.seek_frames(50000);
    session.toggle_reverse();
    session.engine().render(out.data(), 1000);
    CHECK(session.reverse());
    CHECK_FALSE(session.is_playing());
    session.toggle_reverse();
    session.engine().render(out.data(), 1000);
    CHECK_FALSE(session.reverse());
    CHECK_FALSE(session.is_playing());
    CHECK(session.position_frames() == 50000);
}

TEST_CASE("Reverse switches itself off at the start of the timeline") {
    for (const bool option : {true, false}) {
        toh::app::Session session;
        auto clip = std::make_shared<ClipData>();
        clip->channels = 2;
        clip->sample_rate = kRate;
        clip->samples.assign(2 * 10000, 0.5f);
        REQUIRE(session.add_track("track", clip));
        session.set_reverse_off_at_start(option);
        std::vector<float> out(2 * 1000);

        session.seek_frames(3000);
        session.toggle_reverse();
        session.engine().render(out.data(), 1000);
        session.update();
        CHECK(session.reverse());
        CHECK(session.is_playing());
        for (int block = 0; block < 5; ++block) {
            session.engine().render(out.data(), 1000);
            session.update();
        }
        CHECK_FALSE(session.is_playing());
        CHECK(session.position_frames() == 0);
        CHECK(session.reverse() == !option);
    }
}

TEST_CASE("The reverse button starts playback from the playhead") {
    toh::app::Session session;
    auto clip = std::make_shared<ClipData>();
    clip->channels = 2;
    clip->sample_rate = kRate;
    clip->samples.assign(2 * 100000, 0.5f);
    REQUIRE(session.add_track("track", clip));

    const auto run = [&session](std::uint32_t frames) {
        std::vector<float> out(static_cast<std::size_t>(frames) * 2);
        session.engine().render(out.data(), frames);
        return out;
    };

    // Stopped at a chosen position: one press plays backwards from there.
    session.seek_frames(50000);
    session.toggle_reverse();
    auto out = run(1000);
    CHECK(session.reverse());
    CHECK(session.is_playing());
    CHECK(session.position_frames() == 49000);
    CHECK(out.back() == doctest::Approx(0.5f));

    // Paused: the next press turns around and resumes, forwards this time.
    session.pause();
    run(1000);
    CHECK_FALSE(session.is_playing());
    const std::int64_t paused_at = session.position_frames();
    session.toggle_reverse();
    run(1000);
    CHECK_FALSE(session.reverse());
    CHECK(session.is_playing());
    CHECK(session.position_frames() == paused_at + 1000);

    // Pressed in the middle of a pause fade-out: it still ends up playing the new direction.
    session.pause();
    run(50);
    session.toggle_reverse();
    run(2000);
    CHECK(session.is_playing());
    const std::int64_t before = session.position_frames();
    run(500);
    CHECK(session.position_frames() == before - 500);
}
