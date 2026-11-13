// Tests for monitoring, recording and export.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/audio/clip_loader.h"
#include "toh/core/clip.h"
#include "toh/core/engine.h"

using toh::app::Session;
using toh::audio::SampleFormat;
using toh::core::ClipData;
using toh::core::Engine;

namespace {

constexpr std::uint32_t kRate = 48000;

std::shared_ptr<ClipData> constant_clip(std::size_t frames, std::uint32_t channels, float value) {
    auto clip = std::make_shared<ClipData>();
    clip->channels = channels;
    clip->sample_rate = kRate;
    clip->samples.assign(frames * channels, value);
    return clip;
}

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

// Stands in for a sound card: produces a numbered input signal and collects the output.
struct FakeCard {
    explicit FakeCard(std::uint32_t channel_count) : channels(channel_count) {}

    // The value the input has at a given input frame (the same on every channel).
    static float input_value(std::int64_t frame) { return static_cast<float>(frame % 50000) * 0.00001f; }

    std::vector<float> run(Engine& engine, std::uint32_t frames) {
        std::vector<float> in(static_cast<std::size_t>(frames) * channels);
        for (std::size_t i = 0; i < frames; ++i) {
            for (std::size_t c = 0; c < channels; ++c) {
                in[i * channels + c] = input_value(next_input + static_cast<std::int64_t>(i));
            }
        }
        next_input += frames;
        std::vector<float> out(in.size(), 123.0f);
        engine.render(in.data(), out.data(), frames);
        return out;
    }

    std::uint32_t channels;
    std::int64_t next_input = 0;
};

std::vector<float> read_all(Engine& engine) {
    std::vector<float> all;
    std::vector<float> block(1000 * engine.channels());
    for (;;) {
        const std::size_t frames = engine.read_recorded(block.data(), 1000);
        if (frames == 0) {
            break;
        }
        all.insert(all.end(), block.begin(),
                   block.begin() + static_cast<std::ptrdiff_t>(frames * engine.channels()));
    }
    return all;
}

std::filesystem::path temp_file(const char* name) {
    return std::filesystem::temp_directory_path() / std::filesystem::u8path(name);
}

}  // namespace

TEST_CASE("Recording captures the input in step with the timeline") {
    Engine engine(kRate, 1);
    FakeCard card(1);

    card.run(engine, 500);  // not armed: nothing is recorded
    CHECK(read_all(engine).empty());
    CHECK_FALSE(engine.is_recording());

    REQUIRE(engine.start_recording());
    REQUIRE(engine.play());  // runs although nothing is loaded
    const auto out = card.run(engine, 3000);
    CHECK(engine.is_recording());
    CHECK(engine.is_playing());
    CHECK(engine.record_start_frame() == 0);
    CHECK(engine.position_frames() == 3000);
    for (const float sample : out) {
        CHECK(sample == 0.0f);  // the input is not heard unless monitoring is on
    }

    auto recorded = read_all(engine);
    REQUIRE(recorded.size() == 3000);
    for (std::size_t i = 0; i < recorded.size(); ++i) {
        CHECK(recorded[i] == FakeCard::input_value(500 + static_cast<std::int64_t>(i)));
    }

    // Pausing ends the take once the short fade-out is over.
    REQUIRE(engine.pause());
    card.run(engine, 1000);
    CHECK_FALSE(engine.is_recording());
    recorded = read_all(engine);
    CHECK(recorded.size() > 0);
    CHECK(recorded.size() <= engine.fade_frames());
    for (std::size_t i = 0; i < recorded.size(); ++i) {
        CHECK(recorded[i] == FakeCard::input_value(3500 + static_cast<std::int64_t>(i)));
    }
    CHECK(engine.record_dropped_frames() == 0);
}

TEST_CASE("Recording starts where the playhead is and runs past the end of the project") {
    Engine engine(kRate, 1);
    FakeCard card(1);
    REQUIRE(engine.set_clip(constant_clip(6000, 1, 0.25f)));
    REQUIRE(engine.seek(5000));
    REQUIRE(engine.start_recording());
    REQUIRE(engine.play());

    card.run(engine, 3000);  // the clip ends at frame 6000, recording carries on
    CHECK(engine.is_playing());
    CHECK(engine.is_recording());
    CHECK(engine.record_start_frame() == 5000);
    CHECK(engine.position_frames() == 8000);
    CHECK(read_all(engine).size() == 3000);

    // stop_recording ends the take at once; playback is a separate matter.
    REQUIRE(engine.stop_recording());
    card.run(engine, 100);
    CHECK_FALSE(engine.is_recording());
    CHECK(read_all(engine).empty());
}

TEST_CASE("A jump of the playhead ends the take") {
    Engine engine(kRate, 1);
    FakeCard card(1);
    REQUIRE(engine.set_clip(constant_clip(100000, 1, 0.25f)));
    REQUIRE(engine.start_recording());
    REQUIRE(engine.play());
    card.run(engine, 1000);
    REQUIRE(engine.seek(50000));
    card.run(engine, 2000);
    CHECK_FALSE(engine.is_recording());
    CHECK(engine.is_playing());
    const auto recorded = read_all(engine);
    CHECK(recorded.size() >= 1000);
    CHECK(recorded.size() <= 1000 + engine.fade_frames());
}

TEST_CASE("Recording counts input that nobody fetched in time") {
    Engine engine(kRate, 1);
    FakeCard card(1);
    REQUIRE(engine.start_recording());
    REQUIRE(engine.play());
    for (int i = 0; i < 10; ++i) {
        card.run(engine, 48000);  // ten seconds without reading; the buffer holds about four
    }
    CHECK(engine.record_dropped_frames() > 0);
    const auto recorded = read_all(engine);
    CHECK(recorded.size() >= 4 * 48000);
    CHECK(recorded.size() + engine.record_dropped_frames() == 480000);
}

TEST_CASE("Monitoring passes the input to the output") {
    Engine engine(kRate, 2);
    std::vector<float> in(2 * 4800, 0.4f);
    std::vector<float> out(in.size());

    engine.render(in.data(), out.data(), 4800);
    CHECK(out.back() == 0.0f);

    REQUIRE(engine.set_monitoring(true, 0.5f));
    engine.render(in.data(), out.data(), 4800);
    CHECK(out.back() == doctest::Approx(0.2f).epsilon(0.001));
    float largest_step = 0.0f;
    for (std::size_t i = 2; i < out.size(); i += 2) {
        largest_step = std::max(largest_step, std::fabs(out[i] - out[i - 2]));
    }
    CHECK(largest_step < 0.01f);  // it is switched on smoothly

    REQUIRE(engine.set_monitoring(false));
    engine.render(in.data(), out.data(), 4800);
    engine.render(in.data(), out.data(), 4800);  // it is switched off smoothly as well
    CHECK(out.back() == 0.0f);
    CHECK_FALSE(engine.set_monitoring(true, -1.0f));
}

TEST_CASE("Playback can start without the fade-in") {
    Engine engine(kRate, 1);
    REQUIRE(engine.set_clip(constant_clip(1000, 1, 1.0f)));
    REQUIRE(engine.play(false));
    std::vector<float> out(100);
    engine.render(out.data(), 100);
    CHECK(out[0] == 1.0f);
    CHECK(out[99] == 1.0f);
}

TEST_CASE("Session turns a take into a region at the right place") {
    Session session;  // stereo, no sound device: the test plays the sound card
    FakeCard card(2);
    session.set_record_offset_frames(0);

    REQUIRE(session.start_recording());
    CHECK_FALSE(session.start_recording());  // already running
    REQUIRE(session.tracks().size() == 1);   // a new track was made for the take
    session.update();
    card.run(session.engine(), 4800);
    session.update();
    CHECK(session.is_recording());
    CHECK(session.recorded_frames() == 4800);

    session.stop_recording();
    session.update();
    card.run(session.engine(), 1000);
    session.update();
    CHECK_FALSE(session.is_recording());

    REQUIRE(session.tracks()[0].regions.size() == 1);
    const auto first = session.tracks()[0].regions[0];
    CHECK(first.start == 0);
    CHECK(first.length >= 4800);
    CHECK(first.length <= 4800 + 300);
    CHECK(session.length_frames() == first.length);
    for (std::size_t i = 0; i < 4800; i += 97) {
        float value[2] = {};
        REQUIRE(toh::core::read_clip_frames(*first.source->clip, static_cast<std::int64_t>(i), 1, value) == 1);
        CHECK(value[0] == FakeCard::input_value(static_cast<std::int64_t>(i)));
    }

    // Played back, the take sits exactly where it was recorded.
    session.stop();
    card.run(session.engine(), 600);
    session.play();
    const auto out = card.run(session.engine(), 4000);
    for (std::size_t i = 500; i < 4000; i += 97) {
        CHECK(out[2 * i] == doctest::Approx(FakeCard::input_value(static_cast<std::int64_t>(i))));
    }

    // A second take onto the same track, placed 100 frames earlier to make up for latency.
    session.stop();
    card.run(session.engine(), 600);
    session.update();
    session.seek_frames(2000);
    session.set_record_offset_frames(100);
    REQUIRE(session.start_recording(0));
    session.update();
    card.run(session.engine(), 1000);
    session.update();
    session.stop_recording();
    session.update();
    card.run(session.engine(), 1000);
    session.update();
    REQUIRE(session.tracks()[0].regions.size() == 2);
    CHECK(session.tracks()[0].regions[1].start == 1900);
    CHECK(session.tracks()[0].regions[1].offset == 0);
}

TEST_CASE("A take that would start before the timeline is trimmed instead") {
    Session session;
    FakeCard card(2);
    session.set_record_offset_frames(256);
    REQUIRE(session.start_recording());
    session.update();
    card.run(session.engine(), 2000);
    session.update();
    session.stop_recording();
    session.update();
    card.run(session.engine(), 1000);
    session.update();

    REQUIRE(session.tracks()[0].regions.size() == 1);
    const auto& region = session.tracks()[0].regions[0];
    CHECK(region.start == 0);
    CHECK(region.offset == 256);
    CHECK(region.length >= 2000 - 256);
}

TEST_CASE("Export writes the project as it sounds") {
    Session session;
    std::string error;
    const auto path = temp_file("toh_test_\xD1\x8D\xD0\xBA\xD1\x81\xD0\xBF\xD0\xBE\xD1\x80\xD1\x82.wav");

    CHECK_FALSE(session.export_wav(path, SampleFormat::Float32, &error));  // nothing to export
    CHECK_FALSE(error.empty());

    REQUIRE(session.add_track("ramp", ramp_clip(10000, 2, 0.0001f)));
    REQUIRE(session.add_track("step", constant_clip(3000, 2, 0.25f)));
    REQUIRE(session.set_track_gain(0, 0.5f));
    REQUIRE(session.move_region(1, 0, 2000));

    REQUIRE(session.export_wav(path, SampleFormat::Float32, &error));
    auto file = toh::audio::load_clip(path, kRate, 2, &error);
    REQUIRE(file);
    CHECK(file->frames() == 10000);
    const auto expected = [](std::size_t frame) {
        const float ramp = static_cast<float>(frame) * 0.0001f * 0.5f;
        return frame >= 2000 && frame < 5000 ? ramp + 0.25f : ramp;
    };
    for (const std::size_t frame : {std::size_t{0}, std::size_t{1}, std::size_t{100},
                                    std::size_t{1999}, std::size_t{2000}, std::size_t{4999},
                                    std::size_t{5000}, std::size_t{9999}}) {
        CHECK(file->samples[2 * frame] == doctest::Approx(expected(frame)));
        CHECK(file->samples[2 * frame + 1] == doctest::Approx(expected(frame)));
    }

    // Muted tracks are left out, like in playback.
    REQUIRE(session.set_track_mute(1, true));
    REQUIRE(session.export_wav(path, SampleFormat::Int24, &error));
    file = toh::audio::load_clip(path, kRate, 2, &error);
    REQUIRE(file);
    CHECK(file->samples[2 * 3000] == doctest::Approx(0.15f).epsilon(0.0001));

    // Integer files cannot hold levels above full scale: they are limited, not wrapped around.
    REQUIRE(session.set_track_mute(1, false));
    REQUIRE(session.set_master_gain(8.0f));
    REQUIRE(session.export_wav(path, SampleFormat::Int16, &error));
    file = toh::audio::load_clip(path, kRate, 2, &error);
    REQUIRE(file);
    CHECK(file->frames() == 10000);
    CHECK(file->samples[2 * 3000] > 0.99f);
    CHECK(file->samples[2 * 3000] <= 1.0f);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST_CASE("The waveform of a take grows while recording and shows overloads") {
    Session session;
    session.set_record_offset_frames(0);
    REQUIRE(session.start_recording());
    session.update();

    // Quiet input first: a waveform, but no overload.
    std::vector<float> quiet(2 * 2000, 0.3f);
    std::vector<float> out(quiet.size());
    session.engine().render(quiet.data(), out.data(), 2000);
    session.update();
    const auto& wave = session.take_overview();
    CHECK(wave.maximum.size() == (2000 + 255) / 256);
    float low = 0.0f;
    float high = 0.0f;
    REQUIRE(wave.range(0, 1999, low, high));
    CHECK(high == doctest::Approx(0.3f));
    CHECK_FALSE(session.take_clipped());

    // Then full scale: the take is marked as overloaded.
    std::vector<float> loud(2 * 1000, 1.0f);
    session.engine().render(loud.data(), out.data(), 1000);
    session.update();
    CHECK(session.take_overview().maximum.size() == (3000 + 255) / 256);
    CHECK(session.take_clipped());

    // A new take starts with a clean waveform.
    session.stop_recording();
    session.update();
    session.engine().render(quiet.data(), out.data(), 1000);
    session.update();
    CHECK_FALSE(session.is_recording());
    REQUIRE(session.start_recording(0));
    CHECK(session.take_overview().maximum.empty());
    CHECK_FALSE(session.take_clipped());
}
