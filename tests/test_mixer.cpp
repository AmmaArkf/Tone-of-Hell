#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "doctest.h"
#include "toh/core/clip.h"
#include "toh/core/engine.h"

using toh::core::ClipData;
using toh::core::Engine;

namespace {

constexpr std::uint32_t kRate = 48000;

std::shared_ptr<ClipData> constant_clip(std::size_t frames, float value, std::uint32_t channels) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = channels;
    clip->sample_rate = kRate;
    clip->samples.assign(frames * channels, value);
    return clip;
}

std::vector<float> render(Engine& engine, std::uint32_t frames) {
    std::vector<float> out(static_cast<std::size_t>(frames) * engine.channels(), 123.0f);
    engine.render(out.data(), frames);
    return out;
}

// Plays until the fade-in is over and returns the last frame (one value per channel).
std::vector<float> settled_frame(Engine& engine) {
    const auto out = render(engine, 4800);
    return std::vector<float>(out.end() - engine.channels(), out.end());
}

}  // namespace

TEST_CASE("Mixer plays eight tracks in sync and sums them without clipping") {
    Engine engine(kRate, 2);
    const auto clip = toh::core::make_sine_clip(441.0, 1.0, kRate, 2, 0.1f);
    for (std::uint32_t t = 0; t < 8; ++t) {
        REQUIRE(engine.set_track_clip(t, clip));
    }
    REQUIRE(engine.play());

    const std::uint32_t frames = 20000;
    const auto out = render(engine, frames);

    // If any track were even one frame early or late, the sum would differ from eight times
    // the clip.
    const std::size_t first = static_cast<std::size_t>(engine.fade_frames()) + 1;
    float worst = 0.0f;
    for (std::size_t i = first; i < frames; ++i) {
        for (std::size_t c = 0; c < 2; ++c) {
            worst = std::max(worst, std::fabs(out[2 * i + c] - 8.0f * clip->samples[2 * i + c]));
        }
    }
    CHECK(worst < 1.0e-5f);

    CHECK(engine.master_clip_count() == 0);
    CHECK(engine.master_peak() == doctest::Approx(0.8f).epsilon(0.01));
    CHECK(engine.track_peak(0) == doctest::Approx(0.1f).epsilon(0.01));
    CHECK(engine.track_peak(7) == doctest::Approx(0.1f).epsilon(0.01));
    CHECK(engine.track_peak(8) == 0.0f);
    CHECK(engine.position_frames() == frames);
}

TEST_CASE("Mixer reports levels above full scale and does not distort them") {
    Engine engine(kRate, 1);
    for (std::uint32_t t = 0; t < 8; ++t) {
        REQUIRE(engine.set_track_clip(t, constant_clip(10000, 0.25f, 1)));
    }
    REQUIRE(engine.play());
    const auto out = render(engine, 4800);

    CHECK(out.back() == doctest::Approx(2.0f));  // float mixing keeps the headroom
    CHECK(engine.master_clip_count() > 0);
    CHECK(engine.master_peak() == doctest::Approx(2.0f));

    // Pulling the master fader down brings the sum back into range.
    REQUIRE(engine.set_gain(0.4f));
    const auto quieter = render(engine, 4800);
    CHECK(quieter.back() == doctest::Approx(0.8f).epsilon(0.001));
}

TEST_CASE("Mixer applies the track fader") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_track_clip(0, constant_clip(100000, 0.5f, 1)));
    REQUIRE(engine.set_track_clip(1, constant_clip(100000, 0.25f, 1)));
    REQUIRE(engine.set_track_gain(0, 0.5f));
    REQUIRE(engine.set_track_gain(1, 2.0f));
    REQUIRE(engine.play());
    CHECK(settled_frame(engine)[0] == doctest::Approx(0.75f));

    // A fader move during playback glides instead of jumping.
    REQUIRE(engine.set_track_gain(0, 0.0f));
    const auto out = render(engine, 4800);
    float largest_step = 0.0f;
    float previous = 0.75f;
    for (const float sample : out) {
        largest_step = std::max(largest_step, std::fabs(sample - previous));
        previous = sample;
    }
    CHECK(largest_step < 0.005f);
    CHECK(out.back() == doctest::Approx(0.5f).epsilon(0.001));
}

TEST_CASE("Mixer pans a stereo track") {
    Engine engine(kRate, 2);
    REQUIRE(engine.set_track_clip(0, constant_clip(200000, 0.5f, 2)));

    REQUIRE(engine.play());
    auto frame = settled_frame(engine);  // centre: unchanged
    CHECK(frame[0] == doctest::Approx(0.5f));
    CHECK(frame[1] == doctest::Approx(0.5f));

    REQUIRE(engine.set_track_pan(0, -1.0f));
    frame = settled_frame(engine);
    CHECK(frame[0] == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(std::fabs(frame[1]) < 0.001f);

    REQUIRE(engine.set_track_pan(0, 1.0f));
    frame = settled_frame(engine);
    CHECK(std::fabs(frame[0]) < 0.001f);
    CHECK(frame[1] == doctest::Approx(0.5f).epsilon(0.001));

    REQUIRE(engine.set_track_pan(0, 0.5f));
    frame = settled_frame(engine);
    CHECK(frame[0] < 0.4f);
    CHECK(frame[0] > 0.1f);
    CHECK(frame[1] == doctest::Approx(0.5f).epsilon(0.001));
}

TEST_CASE("Mixer mute and solo") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_track_clip(0, constant_clip(400000, 0.25f, 1)));
    REQUIRE(engine.set_track_clip(1, constant_clip(400000, 0.5f, 1)));
    REQUIRE(engine.play());
    CHECK(settled_frame(engine)[0] == doctest::Approx(0.75f));

    REQUIRE(engine.set_track_mute(0, true));
    CHECK(settled_frame(engine)[0] == doctest::Approx(0.5f).epsilon(0.001));

    REQUIRE(engine.set_track_mute(0, false));
    REQUIRE(engine.set_track_solo(0, true));
    CHECK(settled_frame(engine)[0] == doctest::Approx(0.25f).epsilon(0.001));

    REQUIRE(engine.set_track_solo(1, true));  // both soloed: both heard
    CHECK(settled_frame(engine)[0] == doctest::Approx(0.75f).epsilon(0.001));

    REQUIRE(engine.set_track_mute(1, true));  // mute wins over solo
    CHECK(settled_frame(engine)[0] == doctest::Approx(0.25f).epsilon(0.001));

    REQUIRE(engine.set_track_mute(1, false));
    REQUIRE(engine.set_track_solo(0, false));
    REQUIRE(engine.set_track_solo(1, false));
    CHECK(settled_frame(engine)[0] == doctest::Approx(0.75f).epsilon(0.001));
}

TEST_CASE("Mixer routes tracks through buses") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_track_clip(0, constant_clip(400000, 0.25f, 1)));
    REQUIRE(engine.set_track_clip(1, constant_clip(400000, 0.25f, 1)));
    REQUIRE(engine.set_track_clip(2, constant_clip(400000, 0.125f, 1)));
    REQUIRE(engine.set_track_output(0, 3));
    REQUIRE(engine.set_track_output(1, 3));
    REQUIRE(engine.set_bus_gain(3, 0.5f));
    REQUIRE(engine.play());

    // (0.25 + 0.25) * 0.5 through the bus, plus 0.125 straight to the master.
    CHECK(settled_frame(engine)[0] == doctest::Approx(0.375f));
    CHECK(engine.bus_peak(3) == doctest::Approx(0.25f).epsilon(0.01));
    CHECK(engine.bus_peak(0) == 0.0f);

    REQUIRE(engine.set_bus_mute(3, true));
    CHECK(settled_frame(engine)[0] == doctest::Approx(0.125f).epsilon(0.001));

    REQUIRE(engine.set_bus_mute(3, false));
    REQUIRE(engine.set_track_output(0, Engine::kMasterOutput));
    CHECK(settled_frame(engine)[0] == doctest::Approx(0.5f).epsilon(0.001));
}

TEST_CASE("Mixer plays to the end of the longest track") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_track_clip(0, constant_clip(1000, 0.5f, 1)));
    REQUIRE(engine.set_track_clip(5, constant_clip(3000, 0.25f, 1)));
    REQUIRE(engine.play());

    const auto out = render(engine, 4000);
    CHECK(engine.length_frames() == 3000);
    CHECK(out[900] == doctest::Approx(0.75f));
    CHECK(out[1500] == doctest::Approx(0.25f));  // the short track has ended
    CHECK(out[2999] == doctest::Approx(0.25f));
    CHECK(out[3000] == 0.0f);
    CHECK_FALSE(engine.is_playing());

    REQUIRE(engine.clear_track(5));
    render(engine, 16);
    CHECK(engine.length_frames() == 1000);
}

TEST_CASE("Mixer meters fall back after the sound stops") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_track_clip(0, constant_clip(100000, 0.5f, 1)));
    REQUIRE(engine.play());
    render(engine, 4800);
    CHECK(engine.track_peak(0) == doctest::Approx(0.5f));
    CHECK(engine.master_peak() == doctest::Approx(0.5f));

    REQUIRE(engine.stop());
    render(engine, kRate * 3);
    CHECK(engine.track_peak(0) < 0.001f);
    CHECK(engine.master_peak() < 0.001f);
}

TEST_CASE("Mixer rejects invalid tracks, buses and values") {
    Engine engine(kRate, 2);
    const auto clip = constant_clip(100, 0.5f, 2);
    CHECK_FALSE(engine.set_track_clip(Engine::kMaxTracks, clip));
    CHECK_FALSE(engine.clear_track(Engine::kMaxTracks));
    CHECK_FALSE(engine.set_track_gain(0, -1.0f));
    CHECK_FALSE(engine.set_track_gain(0, 100.0f));
    CHECK_FALSE(engine.set_track_pan(0, 1.5f));
    CHECK_FALSE(engine.set_track_pan(0, std::nanf("")));
    CHECK_FALSE(engine.set_track_output(0, static_cast<int>(Engine::kMaxBuses)));
    CHECK_FALSE(engine.set_track_output(0, -2));
    CHECK_FALSE(engine.set_bus_gain(Engine::kMaxBuses, 1.0f));
    CHECK_FALSE(engine.set_bus_mute(Engine::kMaxBuses, true));
    CHECK(engine.track_peak(Engine::kMaxTracks) == 0.0f);
    CHECK(engine.bus_peak(Engine::kMaxBuses) == 0.0f);
}
