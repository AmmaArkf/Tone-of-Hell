// Tests for the sampler: finding the note of a sound and playing it at other notes.

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/core/sampler.h"

using toh::app::Session;
using toh::core::Note;

namespace {

constexpr double kRate = 48000.0;
constexpr double kPi = 3.14159265358979323846;

// A tone of `seconds` with the given frequencies and levels.
std::vector<float> tone(std::vector<std::pair<double, double>> partials, double seconds) {
    std::vector<float> samples(static_cast<std::size_t>(seconds * kRate));
    for (std::size_t i = 0; i < samples.size(); ++i) {
        double value = 0.0;
        for (const auto& [hz, level] : partials) {
            value += level * std::sin(2.0 * kPi * hz * static_cast<double>(i) / kRate);
        }
        samples[i] = static_cast<float>(0.4 * value);
    }
    return samples;
}

}  // namespace

TEST_CASE("The note of a pure tone is found") {
    const std::vector<float> a3 = tone({{220.0, 1.0}}, 1.0);
    const auto analysis = toh::core::analyze_pitch(a3.data(), a3.size(), kRate);
    CHECK(analysis.frequency == doctest::Approx(220.0).epsilon(0.01));
    CHECK(analysis.note == doctest::Approx(57.0).epsilon(0.005));
    REQUIRE(analysis.spectrum.size() == toh::core::kSpectrumBins);
}

TEST_CASE("A loud overtone does not hide the fundamental") {
    // A2 at 110 Hz, with its second and third harmonics louder than itself.
    const std::vector<float> a2 = tone({{110.0, 0.5}, {220.0, 1.0}, {330.0, 0.7}}, 1.0);
    const auto analysis = toh::core::analyze_pitch(a2.data(), a2.size(), kRate);
    CHECK(std::lround(analysis.note) == 45);
}

TEST_CASE("Silence has no note") {
    const std::vector<float> quiet(48000, 0.0f);
    CHECK(toh::core::analyze_pitch(quiet.data(), quiet.size(), kRate).note < 0.0);
}

TEST_CASE("A sampler note an octave up plays the sound twice as fast") {
    const auto sound = toh::core::make_sampler_sound("a4", tone({{440.0, 1.0}}, 1.0), kRate);
    CHECK(sound->root == doctest::Approx(69.0).epsilon(0.005));
    Note note;
    note.pitch = 81;
    note.length = 48000;
    note.velocity = 1.0f;
    // Count the rising zero crossings in a tenth of a second: about 88 at 880 Hz.
    int crossings = 0;
    float previous = 0.0f;
    for (int i = 1000; i < 1000 + 4800; ++i) {
        const float value = toh::core::sampler_note(*sound, note, i, kRate);
        if (previous < 0.0f && value >= 0.0f) {
            ++crossings;
        }
        previous = value;
    }
    CHECK(crossings >= 86);
    CHECK(crossings <= 90);
    // An octave up the one-second sound is over after half a second.
    CHECK(toh::core::sampler_note(*sound, note, 0.6 * kRate, kRate) == 0.0f);
}

TEST_CASE("A sampler track plays its notes and is saved with the project") {
    Session session;
    session.add_instrument_track("Sampler", toh::core::Instrument::Keys, 0, 48000);
    REQUIRE(session.set_region_notes(0, 0, {Note{0, 24000, 60, 0.9f}}));
    REQUIRE(session.set_track_sampler_sound(
        0, toh::core::make_sampler_sound("hum", tone({{261.63, 1.0}}, 1.0), kRate)));
    REQUIRE(session.set_track_sampler_root(0, 59.5));

    session.play();
    std::vector<float> out(2 * 4800);
    session.engine().render(out.data(), 4800);
    float loudest = 0.0f;
    for (const float sample : out) {
        loudest = std::max(loudest, std::fabs(sample));
    }
    CHECK(loudest > 0.1f);

    const auto folder = std::filesystem::temp_directory_path() / "toh_sampler_test";
    std::filesystem::create_directories(folder);
    const auto file = folder / "sampler.tohp";
    std::string error;
    REQUIRE_MESSAGE(session.save_project(file, &error), error);

    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(file, &error), error);
    REQUIRE(reopened.tracks().size() == 1);
    const auto& sound = reopened.tracks()[0].sampler;
    REQUIRE(sound != nullptr);
    CHECK(sound->name == "hum");
    CHECK(sound->root == doctest::Approx(59.5));
    CHECK(sound->data->size() == 48000);
    std::filesystem::remove_all(folder);
}
