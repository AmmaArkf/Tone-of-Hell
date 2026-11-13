// Tests for hosting CLAP plugins, with the example plugins that come with the program.

#include <cmath>
#include <cstddef>
#include <filesystem>
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
    REQUIRE_MESSAGE(infos.size() == 4, error);
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

    REQUIRE(reopened.remove_effect(0, 0));
    CHECK(reopened.tracks()[0].effects.empty());
    reopened.commit_edits();
    REQUIRE(reopened.undo());
    CHECK(reopened.tracks()[0].effects.size() == 1);
    std::filesystem::remove_all(folder);
}
