// Tests for hosting CLAP plugins, with the example plugins that come with the program.

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/core/clip.h"
#include "toh/plugins/clap_plugin.h"

using toh::app::Session;
using toh::plugins::ClapPlugin;

namespace {

const std::filesystem::path kExamples = std::filesystem::u8path(TOH_EXAMPLE_PLUGINS);

std::shared_ptr<toh::core::ClipData> steady_clip(std::size_t frames, float value) {
    auto clip = std::make_shared<toh::core::ClipData>();
    clip->channels = 2;
    clip->sample_rate = 48000;
    clip->samples.assign(frames * 2, value);
    return clip;
}

// The left sample at frame 900 of a session played from the start.
float played_sample(Session& session) {
    session.play();
    std::vector<float> out(2 * 512);
    session.engine().render(out.data(), 512);
    session.engine().render(out.data(), 512);
    return out[2 * (900 - 512)];
}

}  // namespace

TEST_CASE("The example plugins are found in their file") {
    std::string error;
    const auto infos = toh::plugins::read_clap_file(kExamples, &error);
    REQUIRE_MESSAGE(infos.size() >= 6, error);
    CHECK(infos[4].name == "Noise Gate");
    CHECK(infos[5].name == "Denoiser");
    CHECK(infos[0].name == "Echo");
    CHECK(infos[1].name == "Tremolo");
    CHECK(infos[2].name == "Gain");
    CHECK(infos[3].name == "Simple Synth");
    for (const auto& info : infos) {
        CHECK(info.instrument == (info.name == "Simple Synth"));
        CHECK(info.vendor == "Tone of Hell");
    }
    const auto found = toh::plugins::scan_clap_folders({kExamples.parent_path()});
    CHECK(found.size() >= 4);
}

TEST_CASE("A CLAP effect changes the sound and keeps its state") {
    std::string error;
    auto gain = ClapPlugin::create(kExamples, "org.toneofhell.gain", 48000.0, 2, &error);
    REQUIRE_MESSAGE(gain != nullptr, error);
    REQUIRE(gain->params().size() == 2);
    CHECK(gain->params()[0].name == "Gain");
    CHECK(gain->param_value(0) == doctest::Approx(0.0));
    CHECK(gain->param_text(0, 6.0) == "6.0 dB");

    REQUIRE(gain->set_param(0, 6.0206));
    CHECK(gain->param_value(0) == doctest::Approx(6.0206));  // shown before the plugin has it
    std::vector<float> block(2 * 256, 0.25f);
    gain->process(block.data(), 256, 2);
    CHECK(block[0] == doctest::Approx(0.5f).epsilon(0.001));
    CHECK(block[1] == doctest::Approx(0.5f).epsilon(0.001));
    gain->idle();
    CHECK(gain->param_value(0) == doctest::Approx(6.0206));

    std::vector<char> state;
    REQUIRE(gain->save_state(state));
    auto other = ClapPlugin::create(kExamples, "org.toneofhell.gain", 48000.0, 2, &error);
    REQUIRE(other != nullptr);
    REQUIRE(other->load_state(state));
    CHECK(other->param_value(0) == doctest::Approx(6.0206));
    CHECK_FALSE(other->has_gui());

    CHECK(ClapPlugin::create(kExamples, "org.toneofhell.nothing", 48000.0, 2, &error) == nullptr);
    CHECK_FALSE(error.empty());
}

namespace {

// Plays a second of sound through a plugin; returns the RMS of the last half second.
// The sound is a sine of `tone` plus steady noise of `noise` (both as peak levels).
double rms_through(ClapPlugin& plugin, double tone, double noise, std::uint32_t& seed) {
    constexpr int kBlocks = 94;  // about a second
    std::vector<float> block(2 * 512);
    double sum = 0.0;
    std::size_t counted = 0;
    static std::size_t n = 0;
    for (int b = 0; b < kBlocks; ++b) {
        for (std::size_t i = 0; i < 512; ++i, ++n) {
            seed = seed * 1664525u + 1013904223u;
            const double hiss = (static_cast<double>(seed >> 8) / 8388608.0 - 1.0) * noise;
            const double value = tone * std::sin(2.0 * 3.14159265358979 * 1000.0 * static_cast<double>(n) / 48000.0) + hiss;
            block[2 * i] = block[2 * i + 1] = static_cast<float>(value);
        }
        plugin.process(block.data(), 512, 2);
        plugin.idle();
        if (b >= kBlocks / 2) {
            for (std::size_t i = 0; i < 512; ++i) {
                sum += static_cast<double>(block[2 * i]) * static_cast<double>(block[2 * i]);
                ++counted;
            }
        }
    }
    return std::sqrt(sum / static_cast<double>(counted));
}

}  // namespace

TEST_CASE("The noise gate closes on quiet sound and lets loud sound through") {
    std::string error;
    auto gate = ClapPlugin::create(kExamples, "org.toneofhell.gate", 48000.0, 2, &error);
    REQUIRE_MESSAGE(gate != nullptr, error);
    std::uint32_t seed = 1;
    CHECK(rms_through(*gate, 0.0, 0.001, seed) < 0.00005);  // far below the -45 dB threshold
    const double loud = rms_through(*gate, 0.5, 0.001, seed);
    CHECK(loud == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.03));
}

TEST_CASE("The equalizer is flat at first and turns a band down") {
    std::string error;
    auto eq = ClapPlugin::create(kExamples, "org.toneofhell.equalizer", 48000.0, 2, &error);
    REQUIRE_MESSAGE(eq != nullptr, error);
    REQUIRE(eq->params().size() == 7);
    std::uint32_t seed = 3;
    const double full = 0.5 / std::sqrt(2.0);
    CHECK(rms_through(*eq, 0.5, 0.0, seed) == doctest::Approx(full).epsilon(0.01));
    REQUIRE(eq->set_param(2, -18.0));  // the middle band sits at the 1 kHz of the tone
    CHECK(rms_through(*eq, 0.5, 0.0, seed) == doctest::Approx(full * std::pow(10.0, -18.0 / 20.0)).epsilon(0.05));
    REQUIRE(eq->set_param(2, 0.0));
    REQUIRE(eq->set_param(0, -18.0));  // the low band leaves 1 kHz nearly alone
    CHECK(rms_through(*eq, 0.5, 0.0, seed) == doctest::Approx(full).epsilon(0.05));
}

TEST_CASE("The compressor turns loud sound down and leaves quiet sound alone") {
    std::string error;
    auto compressor = ClapPlugin::create(kExamples, "org.toneofhell.compressor", 48000.0, 2, &error);
    REQUIRE_MESSAGE(compressor != nullptr, error);
    std::uint32_t seed = 5;
    // A tone at -26 dB is below the threshold of -18 dB.
    CHECK(rms_through(*compressor, 0.05, 0.0, seed) == doctest::Approx(0.05 / std::sqrt(2.0)).epsilon(0.02));
    // At -6 dB it is 12 dB over; with 4 : 1 it comes out 3 dB over, 9 dB quieter.
    const double loud = rms_through(*compressor, 0.5, 0.0, seed);
    CHECK(loud == doctest::Approx(0.5 / std::sqrt(2.0) * std::pow(10.0, -9.0 / 20.0)).epsilon(0.08));
    // Make-up gain brings it back up.
    REQUIRE(compressor->set_param(4, 9.0));
    CHECK(rms_through(*compressor, 0.5, 0.0, seed) == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.08));
}

TEST_CASE("The limiter keeps every sample under its ceiling") {
    std::string error;
    auto limiter = ClapPlugin::create(kExamples, "org.toneofhell.limiter", 48000.0, 2, &error);
    REQUIRE_MESSAGE(limiter != nullptr, error);
    CHECK(limiter->latency() == 239);  // 5 ms of look-ahead
    std::uint32_t seed = 9;
    // Quiet sound passes unchanged.
    CHECK(rms_through(*limiter, 0.1, 0.0, seed) == doctest::Approx(0.1 / std::sqrt(2.0)).epsilon(0.01));
    // A tone far over full scale, starting suddenly: nothing comes out above -1 dB.
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    std::vector<float> block(2 * 512);
    float highest = 0.0f;
    double energy = 0.0;
    for (int b = 0; b < 94; ++b) {
        for (std::size_t i = 0; i < 512; ++i) {
            const double t = static_cast<double>(b * 512 + static_cast<int>(i)) / 48000.0;
            block[2 * i] = block[2 * i + 1] = static_cast<float>(1.5 * std::sin(2.0 * 3.14159265358979 * 440.0 * t));
        }
        limiter->process(block.data(), 512, 2);
        for (const float sample : block) {
            highest = std::max(highest, std::fabs(sample));
            energy += static_cast<double>(sample) * static_cast<double>(sample);
        }
    }
    CHECK(highest <= ceiling * 1.0001f);
    CHECK(std::sqrt(energy / (94.0 * 1024.0)) > 0.4);  // limited, not silenced
}

TEST_CASE("The reverb leaves a tail after the sound has ended") {
    std::string error;
    auto reverb = ClapPlugin::create(kExamples, "org.toneofhell.reverb", 48000.0, 2, &error);
    REQUIRE_MESSAGE(reverb != nullptr, error);
    std::uint32_t seed = 11;
    // With the mix at 0 the sound passes unchanged.
    REQUIRE(reverb->set_param(2, 0.0));
    CHECK(rms_through(*reverb, 0.5, 0.0, seed) == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.01));
    reverb->reset();

    // Only the reverb: one click, then silence going in; the tail still sounds a quarter second
    // later and has faded after ten seconds.
    REQUIRE(reverb->set_param(2, 100.0));
    std::vector<float> block(2 * 512, 0.0f);
    block[0] = block[1] = 1.0f;
    float early = 0.0f;
    float late = 0.0f;
    for (int b = 0; b < 1000; ++b) {
        reverb->process(block.data(), 512, 2);
        for (const float sample : block) {
            if (b >= 20 && b < 30) {
                early = std::max(early, std::fabs(sample));
            }
            if (b >= 990) {
                late = std::max(late, std::fabs(sample));
            }
        }
        std::fill(block.begin(), block.end(), 0.0f);
    }
    CHECK(early > 0.0005f);
    CHECK(late < early * 0.01f);
}

TEST_CASE("The denoiser learns a noise and takes it out, keeping the sound") {
    std::string error;
    auto denoiser = ClapPlugin::create(kExamples, "org.toneofhell.denoiser", 48000.0, 2, &error);
    REQUIRE_MESSAGE(denoiser != nullptr, error);
    CHECK(denoiser->latency() == 768);
    std::uint32_t seed = 7;
    // Before it has learned anything the sound passes unchanged.
    const double untouched = rms_through(*denoiser, 0.0, 0.05, seed);
    CHECK(untouched == doctest::Approx(0.05 / std::sqrt(3.0)).epsilon(0.05));

    REQUIRE(denoiser->set_param(0, 1.0));  // learn from a second of the noise alone
    rms_through(*denoiser, 0.0, 0.05, seed);
    REQUIRE(denoiser->set_param(0, 0.0));
    const double cleaned = rms_through(*denoiser, 0.0, 0.05, seed);
    CHECK(cleaned < 0.35 * untouched);  // more than 9 dB quieter

    // A tone in the noise stays as loud as it was.
    const double tone = rms_through(*denoiser, 0.5, 0.05, seed);
    CHECK(tone == doctest::Approx(0.5 / std::sqrt(2.0)).epsilon(0.05));

    // The learned noise is part of the state.
    std::vector<char> state;
    REQUIRE(denoiser->save_state(state));
    auto other = ClapPlugin::create(kExamples, "org.toneofhell.denoiser", 48000.0, 2, &error);
    REQUIRE(other != nullptr);
    REQUIRE(other->load_state(state));
    CHECK(rms_through(*other, 0.0, 0.05, seed) < 0.35 * untouched);
}

TEST_CASE("An instrument plugin plays the notes of its track, live keys too") {
    Session session;
    session.add_instrument_track("Synth", toh::core::Instrument::Keys, 0, 48000);
    REQUIRE(session.set_region_notes(0, 0, {toh::core::Note{0, 24000, 69, 1.0f}}));
    std::string error;
    REQUIRE_MESSAGE(session.set_track_plugin_instrument(0, kExamples, "org.toneofhell.synth", &error), error);
    REQUIRE(session.tracks()[0].plugin_instrument.plugin != nullptr);
    CHECK(session.tracks()[0].plugin_instrument.plugin->is_instrument());

    const auto loudest = [&session](int blocks) {
        std::vector<float> out(2 * 512);
        float peak = 0.0f;
        for (int b = 0; b < blocks; ++b) {
            session.engine().render(out.data(), 512);
            for (const float sample : out) {
                peak = std::max(peak, std::fabs(sample));
            }
        }
        return peak;
    };
    session.play();
    CHECK(loudest(8) > 0.05f);  // the note of the region
    session.stop();
    loudest(40);                 // stopped: the note ends and fades out
    CHECK(loudest(4) < 0.001f);

    // A key played live sounds while playback is stopped, and ends with the key.
    session.set_live_track(0);
    REQUIRE(session.note_on(60, 0.9f));
    CHECK(loudest(8) > 0.05f);
    session.note_off(60);
    loudest(40);
    CHECK(loudest(4) < 0.001f);

    const auto folder = std::filesystem::temp_directory_path() / "toh_instrument_test";
    std::filesystem::create_directories(folder);
    const auto file = folder / "synth.tohp";
    session.tracks()[0].plugin_instrument.plugin->set_param(3, -12.0);
    session.tracks()[0].plugin_instrument.plugin->idle();
    REQUIRE_MESSAGE(session.save_project(file, &error), error);
    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(file, &error), error);
    REQUIRE(reopened.tracks()[0].plugin_instrument.plugin != nullptr);
    CHECK(reopened.tracks()[0].plugin_instrument.name == "Simple Synth");
    std::filesystem::remove_all(folder);
}

TEST_CASE("A parameter of a plugin follows its automation, and the curve is saved") {
    Session plain;
    REQUIRE(plain.add_track("take", steady_clip(48000, 0.25f)));
    const float dry = played_sample(plain);

    Session session;
    REQUIRE(session.add_track("take", steady_clip(48000, 0.25f)));
    std::string error;
    REQUIRE_MESSAGE(session.add_clap_effect(0, kExamples, "org.toneofhell.gain", &error), error);
    const auto plugin = session.tracks()[0].effects[0].plugin;
    REQUIRE(session.set_param_curve(0, plugin, 0, {{0, 6.0206f}, {40000, 99.0f}}));
    REQUIRE(session.tracks()[0].param_curves.size() == 1);
    CHECK(session.tracks()[0].param_curves[0].points[1].value == doctest::Approx(24.0f));  // the limit
    REQUIRE(session.set_param_curve(0, plugin, 0, {{0, 6.0206f}}));
    CHECK(played_sample(session) == doctest::Approx(2.0f * dry).epsilon(0.01));

    const auto folder = std::filesystem::temp_directory_path() / "toh_param_curve_test";
    std::filesystem::create_directories(folder);
    REQUIRE_MESSAGE(session.save_project(folder / "c.tohp", &error), error);
    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(folder / "c.tohp", &error), error);
    REQUIRE(reopened.tracks()[0].param_curves.size() == 1);
    CHECK(reopened.tracks()[0].param_curves[0].plugin == reopened.tracks()[0].effects[0].plugin);
    CHECK(reopened.tracks()[0].param_curves[0].points[0].value == doctest::Approx(6.0206f));
    std::filesystem::remove_all(folder);

    REQUIRE(session.set_param_curve(0, plugin, 0, {}));
    CHECK(session.tracks()[0].param_curves.empty());
}

TEST_CASE("A track plays through its effects, and they are saved with the project") {
    Session plain;
    REQUIRE(plain.add_track("take", steady_clip(48000, 0.25f)));
    const float dry = played_sample(plain);
    REQUIRE(dry > 0.05f);

    Session session;
    REQUIRE(session.add_track("take", steady_clip(48000, 0.25f)));
    std::string error;
    REQUIRE_MESSAGE(session.add_clap_effect(0, kExamples, "org.toneofhell.gain", &error), error);
    REQUIRE(session.tracks()[0].effects.size() == 1);
    session.tracks()[0].effects[0].plugin->set_param(0, 6.0206);
    CHECK(played_sample(session) == doctest::Approx(2.0f * dry).epsilon(0.01));

    // Switched off, the sound passes unchanged.
    session.stop();
    REQUIRE(session.set_effect_enabled(0, 0, false));
    CHECK(played_sample(session) == doctest::Approx(dry).epsilon(0.01));
    REQUIRE(session.set_effect_enabled(0, 0, true));

    const auto folder = std::filesystem::temp_directory_path() / "toh_plugin_test";
    std::filesystem::create_directories(folder);
    const auto file = folder / "effects.tohp";
    REQUIRE_MESSAGE(session.save_project(file, &error), error);

    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(file, &error), error);
    REQUIRE(reopened.tracks().size() == 1);
    REQUIRE(reopened.tracks()[0].effects.size() == 1);
    const auto& slot = reopened.tracks()[0].effects[0];
    CHECK(slot.enabled);
    CHECK(slot.plugin->param_value(0) == doctest::Approx(6.0206));
    CHECK(played_sample(reopened) == doctest::Approx(2.0f * dry).epsilon(0.01));

    // An exported file holds the sound with the effects, and playback has them back afterwards.
    const auto wav = folder / "effects.wav";
    REQUIRE_MESSAGE(reopened.export_wav(wav, toh::audio::SampleFormat::Int16, &error), error);
    {
        std::ifstream in(wav, std::ios::binary);
        in.seekg(44 + 4 * 1000);  // frame 1000 of 16-bit stereo
        unsigned char bytes[2] = {};
        in.read(reinterpret_cast<char*>(bytes), 2);
        const auto value = static_cast<std::int16_t>(bytes[0] | (bytes[1] << 8));
        CHECK(static_cast<float>(value) / 32768.0f == doctest::Approx(2.0f * dry).epsilon(0.02));
    }
    CHECK(played_sample(reopened) == doctest::Approx(2.0f * dry).epsilon(0.01));

    REQUIRE(reopened.remove_effect(0, 0));
    CHECK(reopened.tracks()[0].effects.empty());
    reopened.commit_edits();
    REQUIRE(reopened.undo());
    CHECK(reopened.tracks()[0].effects.size() == 1);
    std::filesystem::remove_all(folder);
}
