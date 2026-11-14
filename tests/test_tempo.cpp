// Tests for the tempo map.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/core/tempo.h"

using toh::app::Session;
using toh::core::TempoMap;

TEST_CASE("A tempo map turns frames into beats and back") {
    // 120 BPM (a beat is 24000 frames), then 60 BPM (48000 frames) from frame 48000 on.
    const TempoMap map(48000.0, 120.0, 4, {{48000, 60.0}, {-5, 90.0}, {70000, 5000.0}});
    CHECK(map.beats_at(24000.0) == doctest::Approx(1.0));
    CHECK(map.beats_at(48000.0) == doctest::Approx(2.0));
    CHECK(map.beats_at(96000.0) == doctest::Approx(3.0));
    CHECK(map.frame_at(3.0) == doctest::Approx(96000.0));
    CHECK(map.frame_at(1.5) == doctest::Approx(36000.0));
    CHECK(map.bpm_at(10.0) == doctest::Approx(120.0));
    CHECK(map.bpm_at(80000.0) == doctest::Approx(60.0));  // the changes out of range were left out
    CHECK(map.beat_frames_at(80000.0) == doctest::Approx(48000.0));
}

TEST_CASE("Snapping and the metronome follow tempo changes, and they are saved") {
    Session session;
    toh::app::MetronomeSettings metronome = session.metronome();
    metronome.enabled = true;
    metronome.bpm = 120.0;
    metronome.volume = 1.0f;
    REQUIRE(session.set_metronome(metronome));
    session.set_grid_step(toh::app::GridStep::Beat);
    REQUIRE(session.set_tempo_change(48000, 60.0));
    CHECK_FALSE(session.set_tempo_change(1000, 5.0));
    REQUIRE(session.tempo_changes().size() == 1);

    // Beats are at 0, 24000, 48000, then 96000, 144000 ...
    CHECK(session.snap_frame(30000) == 24000);
    CHECK(session.snap_frame(70000) == 48000);
    CHECK(session.snap_frame(80000) == 96000);

    // The metronome: no click at 72000 (a beat at the old tempo), one at 96000.
    session.play();
    std::vector<float> out(2 * 512);
    float at_old_beat = 0.0f;
    float at_new_beat = 0.0f;
    for (std::size_t start = 0; start < 100000; start += 512) {
        session.engine().render(out.data(), 512);
        for (std::size_t i = 0; i < 512; ++i) {
            const std::size_t frame = start + i;
            const float level = std::fabs(out[2 * i]);
            if (frame >= 72000 && frame < 72200) {
                at_old_beat = std::max(at_old_beat, level);
            }
            if (frame >= 96000 && frame < 96200) {
                at_new_beat = std::max(at_new_beat, level);
            }
        }
    }
    CHECK(at_old_beat < 0.001f);
    CHECK(at_new_beat > 0.05f);
    session.stop();

    const auto folder = std::filesystem::temp_directory_path() / "toh_tempo_test";
    std::filesystem::create_directories(folder);
    std::string error;
    REQUIRE_MESSAGE(session.save_project(folder / "t.tohp", &error), error);
    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(folder / "t.tohp", &error), error);
    REQUIRE(reopened.tempo_changes().size() == 1);
    CHECK(reopened.tempo_changes()[0].frame == 48000);
    CHECK(reopened.bpm_at(50000) == doctest::Approx(60.0));
    std::filesystem::remove_all(folder);

    REQUIRE(session.remove_tempo_change(0));
    CHECK(session.snap_frame(70000) == 72000);
    session.new_project();
    CHECK(session.tempo_changes().empty());
}
