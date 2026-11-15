// Tests for the library: stock sounds, ready-made parts, and placing them on tracks.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <string>
#include <thread>

#include "doctest.h"
#include "toh/app/controllers.h"
#include "toh/app/library.h"
#include "toh/app/session.h"
#include "toh/core/stock_sounds.h"
#include "toh/core/stretch.h"
#include "toh/core/tempo_detect.h"
#include "toh/core/pitch_track.h"
#include "toh/core/mastering.h"

using toh::app::Session;

TEST_CASE("A project is saved in the background while the program goes on") {
    Session session;
    const std::size_t track = session.add_empty_track("Drums");
    REQUIRE(session.add_clip_region(track, 0, "Kick", toh::core::make_stock_sound(0, 48000, 2)));
    const auto folder = std::filesystem::temp_directory_path() / "toh_background_save";
    std::filesystem::create_directories(folder);
    const auto file = folder / "b.tohp";
    REQUIRE(session.save_project_in_background(file));
    CHECK_FALSE(session.save_project_in_background(file));  // one at a time
    session.add_marker(1000, "Made while saving");
    toh::app::Session::SaveResult result;
    for (int k = 0; k < 2000 && !result.done; ++k) {
        session.update();
        result = session.take_save_result();
        if (!result.done) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    REQUIRE(result.done);
    CHECK(result.ok);
    CHECK_FALSE(session.saving());
    CHECK(session.project_file() == file);
    CHECK(session.modified());  // the marker came after the save began
    Session reopened;
    std::string error;
    REQUIRE_MESSAGE(reopened.open_project(file, &error), error);
    REQUIRE(reopened.tracks().size() == 1);
    CHECK(reopened.markers().empty());
    std::filesystem::remove_all(folder);
}

namespace {

// Upward zero crossings of the first channel, per second.
double crossings_per_second(const std::vector<float>& data, std::size_t channels, double rate) {
    int count = 0;
    const std::size_t frames = data.size() / channels;
    for (std::size_t i = 1; i < frames; ++i) {
        if (data[(i - 1) * channels] < 0.0f && data[i * channels] >= 0.0f) {
            ++count;
        }
    }
    return count / (static_cast<double>(frames) / rate);
}

}  // namespace

TEST_CASE("Stretching keeps the pitch, shifting the pitch keeps the length") {
    const auto tone = toh::core::make_sine_clip(441.0, 1.0, 48000, 2, 0.5f);
    const std::size_t frames = static_cast<std::size_t>(tone->frames());
    const std::vector<float> longer = toh::core::time_stretch(tone->samples.data(), frames, 2, 1.5);
    CHECK(longer.size() == static_cast<std::size_t>(std::llround(static_cast<double>(frames) * 1.5)) * 2);
    CHECK(crossings_per_second(longer, 2, 48000.0) == doctest::Approx(441.0).epsilon(0.03));
    const std::vector<float> higher = toh::core::pitch_shift(tone->samples.data(), frames, 2, 12.0);
    CHECK(higher.size() == tone->samples.size());
    CHECK(crossings_per_second(higher, 2, 48000.0) == doctest::Approx(882.0).epsilon(0.05));
}

TEST_CASE("The sound of a region is normalized, reversed and stretched, and can be undone") {
    Session session;
    REQUIRE(session.add_track("Tone", toh::core::make_sine_clip(441.0, 1.0, 48000, 2, 0.25f)));
    session.commit_edits();
    REQUIRE(session.edit_region_sound(0, 0, toh::app::SoundEdit::Normalize));
    session.commit_edits();
    CHECK(session.undo_label() == "Normalize");
    std::vector<float> data(2 * 48000);
    const auto& region = session.tracks()[0].regions[0];
    toh::core::read_clip_frames(*region.source->clip, 0, 48000, data.data());
    float peak = 0.0f;
    for (const float value : data) {
        peak = std::max(peak, std::fabs(value));
    }
    CHECK(peak == doctest::Approx(0.98f).epsilon(0.01));
    REQUIRE(session.edit_region_sound(0, 0, toh::app::SoundEdit::Stretch, 2.0));
    CHECK(session.tracks()[0].regions[0].length == 96000);
    CHECK_FALSE(session.edit_region_sound(0, 0, toh::app::SoundEdit::Stretch, 9.0));
    session.commit_edits();
    REQUIRE(session.undo());
    CHECK(session.tracks()[0].regions[0].length == 48000);
}

namespace {

// The loudest sample of the last tenth of a second, after a second has played.
float settled_peak(Session& session) {
    session.seek_frames(0);
    session.play();
    std::vector<float> block(2 * 4800);
    float peak = 0.0f;
    for (int k = 0; k < 10; ++k) {
        session.engine().render(block.data(), 4800);
    }
    for (const float value : block) {
        peak = std::max(peak, std::fabs(value));
    }
    session.stop();
    return peak;
}

}  // namespace

TEST_CASE("Tracks in a group sum on its bus, with its fader and mute") {
    Session session;
    REQUIRE(session.add_track("A", toh::core::make_sine_clip(441.0, 3.0, 48000, 2, 0.25f)));
    REQUIRE(session.add_track("B", toh::core::make_sine_clip(441.0, 3.0, 48000, 2, 0.25f)));
    REQUIRE(session.add_track("C", toh::core::make_sine_clip(441.0, 3.0, 48000, 2, 0.25f)));
    session.commit_edits();
    const float all = settled_peak(session);
    REQUIRE(all > 0.5f);

    const std::size_t group = session.add_group("  Backing  ", {0, 1});
    REQUIRE(group == 0);
    session.commit_edits();
    CHECK(session.undo_label() == "Make a group");
    CHECK(session.groups()[0].name == "Backing");
    CHECK(session.group_tracks(0) == std::vector<std::size_t>{0, 1});
    CHECK(settled_peak(session) == doctest::Approx(all).epsilon(0.01));

    REQUIRE(session.set_group_gain(0, 0.5f));
    CHECK(settled_peak(session) == doctest::Approx(all * 2.0f / 3.0f).epsilon(0.02));
    REQUIRE(session.set_group_mute(0, true));
    CHECK(settled_peak(session) == doctest::Approx(all / 3.0f).epsilon(0.02));
    REQUIRE(session.set_group_mute(0, false));
    session.commit_edits();

    // Saved and opened again.
    const auto folder = std::filesystem::temp_directory_path() / "toh_groups";
    std::filesystem::create_directories(folder);
    const auto file = folder / "g.tohp";
    std::string error;
    REQUIRE(session.set_group_collapsed(0, true));
    REQUIRE_MESSAGE(session.save_project(file, &error), error);
    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(file, &error), error);
    REQUIRE(reopened.groups().size() == 1);
    CHECK(reopened.groups()[0].name == "Backing");
    CHECK(reopened.groups()[0].gain == doctest::Approx(0.5f));
    CHECK(reopened.groups()[0].collapsed);
    CHECK(reopened.tracks()[1].group == 0);
    CHECK(reopened.tracks()[2].group == -1);
    CHECK(settled_peak(reopened) == doctest::Approx(all * 2.0f / 3.0f).epsilon(0.02));
    std::filesystem::remove_all(folder);

    // Removing the group sends its tracks to the master again; undo brings it back.
    REQUIRE(session.remove_group(0));
    CHECK(session.tracks()[0].group == -1);
    session.commit_edits();
    CHECK(settled_peak(session) == doctest::Approx(all).epsilon(0.01));
    REQUIRE(session.undo());
    REQUIRE(session.groups().size() == 1);
    CHECK(session.tracks()[1].group == 0);
    CHECK(settled_peak(session) == doctest::Approx(all * 2.0f / 3.0f).epsilon(0.02));
    CHECK_FALSE(session.set_track_group(2, 3));
    REQUIRE(session.set_track_group(2, 0));
    CHECK(session.group_tracks(0).size() == 3);
}

TEST_CASE("Sidechain ducking makes a track quieter while the key is loud") {
    Session session;
    REQUIRE(session.add_track("Pad", toh::core::make_sine_clip(441.0, 3.0, 48000, 2, 0.25f)));
    const float alone = settled_peak(session);
    REQUIRE(alone > 0.2f);
    // The key sits in a muted group: it is not heard, but its level still ducks the pad.
    REQUIRE(session.add_track("Kick", toh::core::make_sine_clip(60.0, 3.0, 48000, 2, 0.5f)));
    REQUIRE(session.add_group("Keys", {1}) == 0);
    REQUIRE(session.set_group_mute(0, true));
    CHECK(settled_peak(session) == doctest::Approx(alone).epsilon(0.01));
    // The key is 24 dB above -30 dB: 18 dB down, limited to 12 dB.
    REQUIRE(session.set_track_ducking(0, 1, -30.0f, 12.0f, 150.0f));
    session.commit_edits();
    CHECK(session.undo_label() == "Sidechain");
    CHECK(settled_peak(session) == doctest::Approx(alone * 0.2512f).epsilon(0.03));
    REQUIRE(session.set_track_gain(1, 0.0f));  // a silent key: no ducking
    CHECK(settled_peak(session) == doctest::Approx(alone).epsilon(0.01));
    REQUIRE(session.set_track_gain(1, 1.0f));
    CHECK_FALSE(session.set_track_ducking(0, 5, -30.0f, 12.0f, 150.0f));
    REQUIRE(session.set_track_ducking(1, 1, -30.0f, 12.0f, 150.0f));
    CHECK(session.tracks()[1].duck_key == -1);  // not by itself

    const auto folder = std::filesystem::temp_directory_path() / "toh_duck";
    std::filesystem::create_directories(folder);
    const auto file = folder / "d.tohp";
    std::string error;
    REQUIRE_MESSAGE(session.save_project(file, &error), error);
    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(file, &error), error);
    CHECK(reopened.tracks()[0].duck_key == 1);
    CHECK(reopened.tracks()[0].duck_amount_db == doctest::Approx(12.0f));
    CHECK(settled_peak(reopened) == doctest::Approx(alone * 0.2512f).epsilon(0.03));
    std::filesystem::remove_all(folder);

    REQUIRE(session.remove_track(1));
    CHECK(session.tracks()[0].duck_key == -1);  // its key is gone
    CHECK(settled_peak(session) == doctest::Approx(alone).epsilon(0.01));
}

TEST_CASE("MIDI controllers are learned and move volumes, mutes and actions") {
    Session session;
    REQUIRE(session.add_track("A", toh::core::make_sine_clip(441.0, 1.0, 48000, 2, 0.25f)));
    toh::app::Controllers controllers;
    std::vector<toh::app::Action> actions;
    controllers.learn(toh::app::ControlTarget::TrackVolume, 0);
    CHECK(controllers.learning());
    CHECK(controllers.handle(session, 0, 7, 0.3f, actions));  // learned, nothing changes yet
    CHECK(session.tracks()[0].gain == doctest::Approx(1.0f));
    CHECK_FALSE(controllers.handle(session, 0, 7, 0.25f, actions));
    CHECK(session.tracks()[0].gain == doctest::Approx(0.5f));
    CHECK_FALSE(controllers.handle(session, 1, 7, 1.0f, actions));  // another channel
    CHECK(session.tracks()[0].gain == doctest::Approx(0.5f));

    controllers.learn(toh::app::ControlTarget::TrackMute, 0);
    controllers.handle(session, 0, 20, 1.0f, actions);
    CHECK_FALSE(session.tracks()[0].mute);
    controllers.handle(session, 0, 20, 0.0f, actions);
    controllers.handle(session, 0, 20, 1.0f, actions);  // pressed
    CHECK(session.tracks()[0].mute);
    controllers.handle(session, 0, 20, 1.0f, actions);  // still held: nothing new
    CHECK(session.tracks()[0].mute);

    controllers.learn(toh::app::ControlTarget::Action, static_cast<int>(toh::app::Action::PlayPause));
    controllers.handle(session, 0, 21, 0.0f, actions);
    controllers.handle(session, 0, 21, 1.0f, actions);
    REQUIRE(actions.size() == 1);
    CHECK(actions[0] == toh::app::Action::PlayPause);

    // A control learned again for another target leaves the old one.
    controllers.learn(toh::app::ControlTarget::MasterVolume, 0);
    controllers.handle(session, 0, 7, 0.5f, actions);
    CHECK(controllers.find(toh::app::ControlTarget::TrackVolume, 0) == nullptr);
    controllers.handle(session, 0, 7, 0.25f, actions);
    CHECK(session.master_gain() == doctest::Approx(0.5f));

    toh::app::Controllers copy;
    copy.from_lines(controllers.to_lines());
    REQUIRE(copy.mappings().size() == controllers.mappings().size());
    const auto* master = copy.find(toh::app::ControlTarget::MasterVolume, 0);
    REQUIRE(master != nullptr);
    CHECK(master->controller == 7);
    copy.from_lines({"99|7|0|0", "bad", "0|200|0|0"});
    CHECK(copy.mappings().empty());
}

namespace {

// The loudest sample between two seconds of playback that is already running.
float peak_between(Session& session, double from, double to) {
    std::vector<float> block(2 * 4800);
    float peak = 0.0f;
    for (int k = 0; k < static_cast<int>(to * 10.0 + 0.5); ++k) {
        session.engine().render(block.data(), 4800);
        if (k >= static_cast<int>(from * 10.0 + 0.5)) {
            for (const float value : block) {
                peak = std::max(peak, std::fabs(value));
            }
        }
    }
    return peak;
}

}  // namespace

TEST_CASE("A clip of the clip grid plays in a loop instead of the timeline") {
    Session session;
    REQUIRE(session.add_track("Tone", toh::core::make_sine_clip(441.0, 0.5, 48000, 2, 0.25f)));
    REQUIRE(session.put_clip(0, 0) == 0);
    REQUIRE(session.clip_slot(0, 0) != nullptr);
    session.commit_edits();
    CHECK(session.undo_label() == "Put a clip into the grid");
    CHECK(settled_peak(session) < 0.001f);  // the timeline ends after half a second

    REQUIRE(session.launch_clip(0, 0));  // stopped: starts now, and starts playback
    CHECK(session.playing_clip(0) == 0);
    CHECK(peak_between(session, 1.5, 2.0) == doctest::Approx(0.25f).epsilon(0.05));
    session.stop();
    session.stop_all_clips();
    CHECK(session.playing_clip(0) == -1);
    CHECK(settled_peak(session) < 0.001f);

    const auto folder = std::filesystem::temp_directory_path() / "toh_clips";
    std::filesystem::create_directories(folder);
    const auto file = folder / "c.tohp";
    std::string error;
    REQUIRE(session.remove_region(0, 0));  // the clip keeps its sound
    REQUIRE_MESSAGE(session.save_project(file, &error), error);
    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(file, &error), error);
    REQUIRE(reopened.clip_slot(0, 0) != nullptr);
    CHECK(reopened.clip_slot(0, 0)->length == 24000);
    reopened.launch_scene(0);
    CHECK(peak_between(reopened, 1.0, 1.5) == doctest::Approx(0.25f).epsilon(0.05));
    std::filesystem::remove_all(folder);
    CHECK(reopened.remove_clip(0, 0));
    CHECK_FALSE(reopened.launch_clip(0, 0));
}

namespace {

// Short clicks (a decaying tone) at `bpm`, the first after `offset` frames.
std::shared_ptr<toh::core::ClipData> click_track(double bpm, double seconds, std::int64_t offset) {
    auto clip = std::make_shared<toh::core::ClipData>();
    clip->channels = 2;
    clip->sample_rate = 48000;
    const auto frames = static_cast<std::size_t>(seconds * 48000.0);
    clip->samples.assign(frames * 2, 0.0f);
    const double period = 60.0 / bpm * 48000.0;
    for (double beat = static_cast<double>(offset); beat < static_cast<double>(frames); beat += period) {
        const auto start = static_cast<std::size_t>(std::llround(beat));
        for (std::size_t i = 0; i < 2400 && start + i < frames; ++i) {
            const float value = 0.6f * std::exp(-static_cast<float>(i) / 300.0f) *
                                std::sin(6.2831853f * 1000.0f * static_cast<float>(i) / 48000.0f);
            clip->samples[(start + i) * 2] = value;
            clip->samples[(start + i) * 2 + 1] = value;
        }
    }
    return clip;
}

}  // namespace

TEST_CASE("Smart tempo finds the tempo of a recording and gives it to the project") {
    for (const double bpm : {90.0, 128.0, 160.0}) {
        CAPTURE(bpm);
        const auto clip = click_track(bpm, 20.0, 4800);
        const auto found = toh::core::detect_tempo(clip->samples.data(), clip->frames(), 2, 48000.0);
        CHECK(found.bpm == doctest::Approx(bpm).epsilon(0.005));
        CHECK(found.first_beat == doctest::Approx(4800.0).epsilon(0.08));
    }
    Session session;
    REQUIRE(session.add_track("Drums", click_track(128.0, 20.0, 4800)));
    double bpm = 0.0;
    std::int64_t first = 0;
    REQUIRE(session.detect_region_tempo(0, 0, &bpm, &first));
    CHECK(bpm == doctest::Approx(128.0).epsilon(0.005));
    REQUIRE(session.adopt_region_tempo(0, 0));
    CHECK(session.metronome().bpm == doctest::Approx(128.0).epsilon(0.005));
    // A beat of the region now falls on a bar line.
    REQUIRE(session.detect_region_tempo(0, 0, &bpm, &first));
    const double bars = static_cast<double>(first) / session.bar_frames();
    CHECK(std::fabs(bars - std::round(bars)) * session.bar_frames() < 400.0);
    session.commit_edits();
    CHECK(session.undo_label() == "Take the tempo of a region");

    // Silence and noise have no beat.
    Session quiet;
    REQUIRE(quiet.add_track("Tone", toh::core::make_sine_clip(441.0, 10.0, 48000, 2, 0.25f)));
    CHECK_FALSE(quiet.detect_region_tempo(0, 0, &bpm));
}

TEST_CASE("The notes of a recorded voice are found and retuned") {
    // Half a second of A3 (220 Hz), then half a second of E4 (329.63 Hz).
    auto clip = std::make_shared<toh::core::ClipData>();
    clip->channels = 2;
    clip->sample_rate = 48000;
    clip->samples.resize(2 * 48000);
    double phase = 0.0;
    for (std::size_t i = 0; i < 48000; ++i) {
        phase += 6.283185307 * (i < 24000 ? 220.0 : 329.63) / 48000.0;
        const float value = 0.4f * static_cast<float>(std::sin(phase));
        clip->samples[2 * i] = value;
        clip->samples[2 * i + 1] = value;
    }
    const auto curve = toh::core::track_pitch(clip->samples.data(), 48000, 2, 48000.0);
    const auto notes = toh::core::find_pitch_notes(curve);
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].pitch == doctest::Approx(57.0f).epsilon(0.003));
    CHECK(notes[1].pitch == doctest::Approx(64.0f).epsilon(0.003));
    CHECK(notes[1].start == doctest::Approx(24000.0).epsilon(0.05));

    Session session;
    REQUIRE(session.add_track("Voice", clip));
    session.commit_edits();
    std::vector<toh::core::PitchNote> found;
    REQUIRE(session.analyse_region_pitch(0, 0, nullptr, &found));
    REQUIRE(found.size() == 2);
    // The first note up a whole tone: B3.
    REQUIRE(session.shift_region_pitch(0, 0, {{found[0].start, found[0].length, 2.0}}));
    session.commit_edits();
    CHECK(session.undo_label() == "Change the pitch of a note");
    CHECK(session.tracks()[0].regions[0].length == 48000);
    REQUIRE(session.analyse_region_pitch(0, 0, nullptr, &found));
    REQUIRE(found.size() >= 2);
    CHECK(found.front().pitch == doctest::Approx(59.0f).epsilon(0.004));
    CHECK(found.back().pitch == doctest::Approx(64.0f).epsilon(0.003));
    REQUIRE(session.undo());
    REQUIRE(session.analyse_region_pitch(0, 0, nullptr, &found));
    CHECK(found.front().pitch == doctest::Approx(57.0f).epsilon(0.003));

    // A drawn pitch line: the whole first note to C4, the second note stays.
    toh::core::PitchCurve line;
    REQUIRE(session.analyse_region_pitch(0, 0, &line, &found));
    std::vector<float> target(line.midi.size(), -1.0f);
    for (std::size_t h = 0; h * line.hop < 24000 && h < target.size(); ++h) {
        target[h] = 60.0f;
    }
    REQUIRE(session.retune_region_pitch(0, 0, target));
    session.commit_edits();
    CHECK(session.undo_label() == "Retune");
    REQUIRE(session.analyse_region_pitch(0, 0, nullptr, &found));
    REQUIRE(found.size() >= 2);
    CHECK(found.front().pitch == doctest::Approx(60.0f).epsilon(0.004));
    CHECK(found.back().pitch == doctest::Approx(64.0f).epsilon(0.003));
    CHECK(session.tracks()[0].regions[0].length == 48000);
    // A line that moves nothing changes nothing.
    std::vector<float> none(line.midi.size(), -1.0f);
    CHECK_FALSE(session.retune_region_pitch(0, 0, none));
}

TEST_CASE("The mastering chain reaches the target loudness under the ceiling") {
    // A 1 kHz sine at -20 dBFS RMS measures -20 LUFS (K-weighting is about 0 dB there, plus the
    // -0.691 offset that the stereo pair makes up for).
    std::vector<float> tone(2 * 48000 * 5);
    for (std::size_t i = 0; i < tone.size() / 2; ++i) {
        const float value = 0.1f * std::sqrt(2.0f) * std::sin(6.2831853f * 1000.0f * static_cast<float>(i) / 48000.0f);
        tone[2 * i] = value;
        tone[2 * i + 1] = value;
    }
    const auto stats = toh::core::measure_mix(tone.data(), tone.size() / 2, 2, 48000.0);
    CHECK(stats.loudness_lufs == doctest::Approx(-17.0).epsilon(0.03));  // two channels: +3 dB
    CHECK(stats.peak_db == doctest::Approx(-17.0).epsilon(0.02));
    CHECK(stats.width == doctest::Approx(0.0));

    // Drum-like hits: loud peaks over a quiet bed.
    std::vector<float> mix(2 * 48000 * 8);
    for (std::size_t i = 0; i < mix.size() / 2; ++i) {
        const double t = static_cast<double>(i) / 48000.0;
        const double hit = std::fmod(t, 0.5);
        const double value = 0.05 * std::sin(6.2831853 * 220.0 * t) + 0.8 * std::exp(-hit * 30.0) * std::sin(6.2831853 * 80.0 * hit);
        mix[2 * i] = static_cast<float>(value);
        mix[2 * i + 1] = static_cast<float>(value * 0.9);
    }
    const auto before = toh::core::measure_mix(mix.data(), mix.size() / 2, 2, 48000.0);
    std::vector<std::string> reasons;
    const auto settings = toh::core::suggest_master(before, -12.0, &reasons);
    CHECK_FALSE(reasons.empty());
    toh::core::master_audio(mix, 2, 48000.0, settings);
    const auto after = toh::core::measure_mix(mix.data(), mix.size() / 2, 2, 48000.0);
    CHECK(after.peak_db <= settings.ceiling_db + 0.01);
    CHECK(after.loudness_lufs == doctest::Approx(-12.0).epsilon(0.1));
    for (const float value : mix) {
        REQUIRE(std::isfinite(value));
    }
}

TEST_CASE("A 5.1 surround export puts every track where it was turned") {
    Session session;
    REQUIRE(session.add_track("Front", toh::core::make_sine_clip(441.0, 1.0, 48000, 2, 0.25f)));
    REQUIRE(session.add_track("Behind", toh::core::make_sine_clip(441.0, 1.0, 48000, 2, 0.25f)));
    REQUIRE(session.set_track_surround(1, 180.0f, 0.5f));
    session.commit_edits();
    CHECK(session.undo_label() == "Surround");
    REQUIRE(session.set_track_mute(0, true));  // first only the track behind
    const auto file = std::filesystem::temp_directory_path() / "toh_surround.wav";
    std::string error;
    REQUIRE_MESSAGE(session.export_surround_wav(file, toh::audio::SampleFormat::Float32, &error), error);
    // 6 channels of 32-bit floats: the size says so.
    CHECK(std::filesystem::file_size(file) >= 6u * 4u * 48000u);
    std::filesystem::remove(file);

    // The placement itself: 180 degrees with the stereo picture 30 degrees to either side lies
    // between the two rear loudspeakers.
    REQUIRE(session.set_track_mute(0, false));
    const auto folder = std::filesystem::temp_directory_path() / "toh_surround_project";
    std::filesystem::create_directories(folder);
    REQUIRE_MESSAGE(session.save_project(folder / "s.tohp", &error), error);
    Session reopened;
    REQUIRE_MESSAGE(reopened.open_project(folder / "s.tohp", &error), error);
    CHECK(reopened.tracks()[1].surround_angle == doctest::Approx(180.0f));
    CHECK(reopened.tracks()[1].surround_lfe == doctest::Approx(0.5f));
    CHECK(reopened.tracks()[0].surround_angle == doctest::Approx(0.0f));
    std::filesystem::remove_all(folder);
}

TEST_CASE("Every stock sound is made, is not silent and is always the same") {
    REQUIRE(toh::core::stock_sound_count() >= 20);
    for (std::size_t k = 0; k < toh::core::stock_sound_count(); ++k) {
        const auto info = toh::core::stock_sound_info(k);
        CAPTURE(std::string(info.name));
        const auto clip = toh::core::make_stock_sound(k, 48000, 2);
        REQUIRE(clip);
        REQUIRE(clip->frames() > 1000);
        float peak = 0.0f;
        for (const float value : clip->samples) {
            REQUIRE(std::isfinite(value));
            peak = std::max(peak, std::fabs(value));
        }
        CHECK(peak == doctest::Approx(0.79f).epsilon(0.01));
        const auto again = toh::core::make_stock_sound(k, 48000, 2);
        CHECK(again->samples == clip->samples);
    }
    CHECK_FALSE(toh::core::make_stock_sound(toh::core::stock_sound_count(), 48000, 2));
}

TEST_CASE("Stock sounds and parts are put onto tracks") {
    Session session;
    const std::size_t audio = session.add_empty_track("Drums");
    REQUIRE(session.add_clip_region(audio, 48000, "Kick", toh::core::make_stock_sound(0, 48000, 2)));
    REQUIRE(session.add_clip_region(audio, 0, "Snare", toh::core::make_stock_sound(3, 48000, 2)));
    REQUIRE(session.tracks()[audio].regions.size() == 2);
    CHECK(session.tracks()[audio].regions[0].start == 0);  // kept in order of time
    CHECK(session.tracks()[audio].regions[1].start == 48000);

    const toh::app::PartInfo& part = toh::app::library_parts().front();
    std::int64_t length = 0;
    const auto notes = toh::app::part_notes(session, part, 96000, &length);
    REQUIRE_FALSE(notes.empty());
    CHECK(length == static_cast<std::int64_t>(std::llround(part.bars * 4.0 * session.beat_frames())));
    for (const auto& note : notes) {
        CHECK(note.start >= 0);
        CHECK(note.start + note.length <= length);
    }
    CHECK_FALSE(session.add_note_region(audio, 0, length, "Beat", notes));  // not on recorded sound
    const std::size_t keys = session.add_instrument_track("Keys", toh::core::Instrument::Keys, 0, 48000, 0);
    REQUIRE(session.add_note_region(keys, 96000, length, "Beat", notes));
    REQUIRE(session.tracks()[keys].regions.size() == 2);
    const auto& placed = session.tracks()[keys].regions[1];
    CHECK(placed.start == 96000);
    CHECK(placed.source->clip->notes->notes.size() == notes.size());
    CHECK_FALSE(session.add_clip_region(keys, 0, "Kick", toh::core::make_stock_sound(0, 48000, 2)));

    for (const toh::app::PartInfo& each : toh::app::library_parts()) {
        CAPTURE(std::string(each.name));
        CHECK_FALSE(each.notes.empty());
        for (const auto& note : each.notes) {
            CHECK(note.beat + note.length <= each.bars * 4.0 + 1e-9);
        }
    }
}
