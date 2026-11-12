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

// A mono clip in which every sample has the same value.
std::shared_ptr<ClipData> constant_clip(std::size_t frames, float value = 1.0f,
                                        std::uint32_t channels = 1) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = channels;
    clip->sample_rate = kRate;
    clip->samples.assign(frames * channels, value);
    return clip;
}

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

std::vector<float> render(Engine& engine, std::uint32_t frames) {
    std::vector<float> out(static_cast<std::size_t>(frames) * engine.channels(), 123.0f);
    engine.render(out.data(), frames);
    return out;
}

float max_step(const std::vector<float>& samples, float previous = 0.0f) {
    float largest = 0.0f;
    for (const float sample : samples) {
        largest = std::max(largest, std::fabs(sample - previous));
        previous = sample;
    }
    return largest;
}

}  // namespace

TEST_CASE("Engine is silent without a clip and before play") {
    Engine engine(kRate, 2);
    CHECK(engine.play());
    for (const float sample : render(engine, 64)) {
        CHECK(sample == 0.0f);
    }
    CHECK_FALSE(engine.is_playing());

    Engine mono(kRate, 1);
    REQUIRE(mono.set_clip(constant_clip(1000)));
    for (const float sample : render(mono, 64)) {
        CHECK(sample == 0.0f);
    }
}

TEST_CASE("Engine rejects clips and arguments that do not fit") {
    Engine engine(kRate, 2);
    CHECK_FALSE(engine.set_clip(nullptr));
    CHECK_FALSE(engine.set_clip(constant_clip(100, 1.0f, 1)));  // wrong channel count
    auto wrong_rate = constant_clip(100, 1.0f, 2);
    wrong_rate->sample_rate = 44100;
    CHECK_FALSE(engine.set_clip(wrong_rate));
    CHECK(engine.set_clip(constant_clip(100, 1.0f, 2)));

    CHECK_FALSE(engine.seek(-1));
    CHECK_FALSE(engine.set_loop(true, 10, 10));
    CHECK_FALSE(engine.set_gain(-0.1f));
    CHECK_FALSE(engine.set_gain(std::nanf("")));
}

TEST_CASE("Engine fades in, then plays the clip unchanged") {
    Engine engine(kRate, 1);
    const std::uint32_t fade = engine.fade_frames();
    REQUIRE(engine.set_clip(constant_clip(10000)));
    REQUIRE(engine.play());

    const auto out = render(engine, fade + 100);
    CHECK(engine.is_playing());
    CHECK(engine.position_frames() == fade + 100);

    CHECK(out[0] > 0.0f);
    CHECK(out[0] < 0.01f);
    for (std::size_t i = 1; i < out.size(); ++i) {
        CHECK(out[i] >= out[i - 1]);
    }
    for (std::size_t i = fade + 1; i < out.size(); ++i) {
        CHECK(out[i] == doctest::Approx(1.0f));
    }
}

TEST_CASE("Engine stops at the end of the clip and can play it again") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_clip(constant_clip(1000)));
    REQUIRE(engine.play());

    const auto out = render(engine, 1500);
    CHECK(out[999] != 0.0f);
    for (std::size_t i = 1000; i < out.size(); ++i) {
        CHECK(out[i] == 0.0f);
    }
    CHECK_FALSE(engine.is_playing());
    CHECK(engine.position_frames() == 1000);

    REQUIRE(engine.play());
    render(engine, 10);
    CHECK(engine.is_playing());
    CHECK(engine.position_frames() == 10);
}

TEST_CASE("Engine loops over the whole clip and over a region") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_clip(index_clip(1000)));
    REQUIRE(engine.set_loop(true));
    REQUIRE(engine.play());

    auto out = render(engine, 2500);
    // After the fade-in the output equals the frame index, wrapping at 1000.
    for (std::size_t i = 500; i < out.size(); ++i) {
        CHECK(out[i] == doctest::Approx(static_cast<float>(i % 1000)));
    }
    CHECK(engine.is_playing());

    REQUIRE(engine.set_loop(true, 100, 200));
    out = render(engine, 1000);  // position was 500: jumps back into the region
    for (const float sample : out) {
        CHECK(sample >= 100.0f - 0.01f);
        CHECK(sample < 200.0f);
    }
}

TEST_CASE("Engine pause keeps the position and stop returns to the start") {
    Engine engine(kRate, 1);
    const std::uint32_t fade = engine.fade_frames();
    REQUIRE(engine.set_clip(constant_clip(100000)));
    REQUIRE(engine.play());
    render(engine, 2000);

    REQUIRE(engine.pause());
    auto out = render(engine, fade + 50);
    CHECK(out.back() == 0.0f);
    CHECK_FALSE(engine.is_playing());
    const std::int64_t paused_at = engine.position_frames();
    CHECK(paused_at > 2000);
    CHECK(paused_at <= 2000 + fade);
    render(engine, 500);
    CHECK(engine.position_frames() == paused_at);

    REQUIRE(engine.play());
    render(engine, 100);
    CHECK(engine.position_frames() == paused_at + 100);

    REQUIRE(engine.stop());
    render(engine, fade + 50);
    CHECK_FALSE(engine.is_playing());
    CHECK(engine.position_frames() == 0);
}

TEST_CASE("Engine seeks while stopped and while playing") {
    Engine engine(kRate, 1);
    const std::uint32_t fade = engine.fade_frames();
    REQUIRE(engine.set_clip(index_clip(50000)));

    REQUIRE(engine.seek(20000));
    render(engine, 16);
    CHECK(engine.position_frames() == 20000);

    REQUIRE(engine.seek(999999));  // past the end: clamped
    render(engine, 16);
    CHECK(engine.position_frames() == 50000);

    REQUIRE(engine.seek(1000));
    REQUIRE(engine.play());
    render(engine, 2000);
    REQUIRE(engine.seek(30000));
    const auto out = render(engine, 3 * fade);
    CHECK(engine.is_playing());
    CHECK(engine.position_frames() > 30000);
    CHECK(engine.position_frames() < 30000 + 3 * fade);
    CHECK(out.back() == doctest::Approx(static_cast<float>(engine.position_frames() - 1)));
}

TEST_CASE("Engine never jumps in level on play, pause, seek, stop or gain changes") {
    Engine engine(kRate, 1);
    const std::uint32_t fade = engine.fade_frames();
    const float limit = 1.5f / static_cast<float>(fade);  // one fade step plus rounding
    REQUIRE(engine.set_clip(constant_clip(200000)));

    std::vector<float> all;
    const auto run = [&](std::uint32_t frames) {
        const auto out = render(engine, frames);
        all.insert(all.end(), out.begin(), out.end());
    };

    REQUIRE(engine.play());
    run(1000);
    REQUIRE(engine.pause());
    run(1000);
    REQUIRE(engine.play());
    run(1000);
    REQUIRE(engine.seek(50000));
    run(1000);
    REQUIRE(engine.set_gain(0.25f));
    run(4000);
    REQUIRE(engine.pause());
    run(100);
    REQUIRE(engine.play());  // resume in the middle of a fade-out
    run(1000);
    REQUIRE(engine.stop());
    run(1000);

    CHECK(max_step(all) <= limit);
    CHECK(all.back() == 0.0f);
}

TEST_CASE("Engine smooths gain changes towards the target") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_clip(constant_clip(100000)));
    REQUIRE(engine.play());
    render(engine, 1000);

    REQUIRE(engine.set_gain(0.5f));
    const auto out = render(engine, 9600);  // 200 ms: twenty time constants
    CHECK(out.front() > 0.9f);
    CHECK(out.back() == doctest::Approx(0.5f).epsilon(0.001));
}

TEST_CASE("Engine frees a replaced clip only after the audio thread let go of it") {
    Engine engine(kRate, 1);
    auto first = constant_clip(1000);
    const std::weak_ptr<ClipData> watch = first;
    REQUIRE(engine.set_clip(first));
    first.reset();
    render(engine, 16);
    engine.collect_retired();
    CHECK_FALSE(watch.expired());  // still the current clip

    REQUIRE(engine.set_clip(constant_clip(1000)));
    engine.collect_retired();
    CHECK_FALSE(watch.expired());  // the audio thread has not swapped yet

    render(engine, 16);
    engine.collect_retired();
    CHECK(watch.expired());
}

TEST_CASE("Engine writes every channel of a stereo clip") {
    Engine engine(kRate, 2);
    auto clip = std::make_shared<ClipData>();
    clip->channels = 2;
    clip->sample_rate = kRate;
    clip->samples.resize(2000 * 2);
    for (std::size_t i = 0; i < 2000; ++i) {
        clip->samples[2 * i] = 0.5f;
        clip->samples[2 * i + 1] = -0.25f;
    }
    REQUIRE(engine.set_clip(clip));
    REQUIRE(engine.play());
    const auto out = render(engine, 1000);
    CHECK(out[2 * 999] == doctest::Approx(0.5f));
    CHECK(out[2 * 999 + 1] == doctest::Approx(-0.25f));
}
