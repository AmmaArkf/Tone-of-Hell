// Tests for note regions and the built-in instruments.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "doctest.h"
#include "toh/app/session.h"
#include "toh/core/engine.h"

using toh::app::Session;
using toh::core::Instrument;
using toh::core::Note;

namespace {

constexpr std::int64_t kRate = 48000;

Note make_note(std::int64_t start, std::int64_t length, int pitch, float velocity = 0.8f) {
    Note note;
    note.start = start;
    note.length = length;
    note.pitch = static_cast<std::uint8_t>(pitch);
    note.velocity = velocity;
    return note;
}

const std::vector<Note>& notes_of(const Session& session, std::size_t track, std::size_t region) {
    return session.tracks()[track].regions[region].source->clip->notes->notes;
}

// Renders `frames` frames and returns the loudest sample of every block of 1000 frames.
std::vector<float> loudness(Session& session, std::int64_t frames) {
    std::vector<float> peaks;
    std::vector<float> out(2 * 1000);
    for (std::int64_t done = 0; done < frames; done += 1000) {
        session.engine().render(out.data(), 1000);
        float peak = 0.0f;
        for (const float sample : out) {
            peak = std::max(peak, std::fabs(sample));
        }
        peaks.push_back(peak);
    }
    return peaks;
}

}  // namespace

TEST_CASE("An instrument track plays its notes and is silent elsewhere") {
    Session session;
    const std::size_t track = session.add_instrument_track("Keys", Instrument::Organ, 0, 4 * kRate);
    REQUIRE(track == 0);
    REQUIRE(session.tracks()[0].regions.size() == 1);
    CHECK(session.tracks()[0].regions[0].has_notes());
    CHECK(session.tracks()[0].instrument == Instrument::Organ);

    // One note from 0.5 s to 1 s.
    REQUIRE(session.set_region_notes(0, 0, {make_note(kRate / 2, kRate / 2, 69)}));
    session.play();
    const std::vector<float> peaks = loudness(session, 2 * kRate);
    CHECK(peaks[10] == 0.0f);   // before the note
    CHECK(peaks[30] > 0.05f);   // while it is held
    CHECK(peaks[60] == 0.0f);   // after its short release
}

TEST_CASE("Every instrument sounds and goes quiet after its release") {
    for (int k = 0; k < toh::core::kInstrumentCount; ++k) {
        const auto instrument = static_cast<Instrument>(k);
        const Note note = make_note(0, kRate / 4, 60);
        float loudest = 0.0f;
        for (int frame = 0; frame < kRate / 4; frame += 7) {
            loudest = std::max(loudest, std::fabs(toh::core::note_sample(instrument, note, frame, kRate)));
        }
        CHECK(loudest > 0.02f);
        CHECK(loudest < 1.0f);
        const double after = kRate / 4 + toh::core::instrument_release_seconds(instrument) * kRate + 1;
        CHECK(toh::core::note_sample(instrument, note, after, kRate) == 0.0f);
        CHECK(toh::core::note_sample(instrument, note, -1.0, kRate) == 0.0f);
    }
}

TEST_CASE("Notes are kept sorted and invalid ones are dropped") {
    Session session;
    session.add_instrument_track("Synth", Instrument::Keys, 1000, 10000);
    REQUIRE(session.set_region_notes(0, 0,
                                     {make_note(500, 100, 60), make_note(100, 0, 61),
                                      make_note(200, 50, 200), make_note(100, 100, 62, 3.0f)}));
    const auto& notes = notes_of(session, 0, 0);
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].start == 100);
    CHECK(notes[0].velocity == doctest::Approx(1.0f));
    CHECK(notes[1].start == 500);

    // Only note regions take notes, and they keep their speed.
    session.add_empty_track("Audio");
    CHECK_FALSE(session.set_region_notes(1, 0, {}));
    CHECK_FALSE(session.set_region_speed(0, 0, 2.0));
}

TEST_CASE("Note regions are cut, joined, undone and saved") {
    Session session;
    session.add_instrument_track("Lead", Instrument::Pluck, 0, 8000);
    REQUIRE(session.set_region_notes(0, 0, {make_note(1000, 500, 60), make_note(5000, 2000, 64)}));
    session.commit_edits();

    // Cutting: each part plays its own notes.
    REQUIRE(session.split_region(0, 0, 4000));
    REQUIRE(session.tracks()[0].regions.size() == 2);
    CHECK(session.tracks()[0].regions[1].offset == 4000);
    REQUIRE(session.move_region(0, 1, 10000));

    // Joining: one note list, placed where the parts were.
    std::size_t joined = 0;
    REQUIRE(session.join_regions(0, {0, 1}, &joined));
    const auto& region = session.tracks()[0].regions[joined];
    REQUIRE(region.has_notes());
    CHECK(region.start == 0);
    CHECK(region.length == 14000);
    const auto& notes = notes_of(session, 0, joined);
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].start == 1000);
    CHECK(notes[1].start == 11000);  // 5000 - 4000 + 10000
    session.commit_edits();

    REQUIRE(session.set_track_instrument(0, Instrument::Pad));
    session.commit_edits();

    // Saved and opened again.
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "toh_test_notes.tohp";
    std::string error;
    REQUIRE_MESSAGE(session.save_project(file, &error), error);
    {
        Session opened;
        REQUIRE_MESSAGE(opened.open_project(file, &error), error);
        REQUIRE(opened.tracks().size() == 1);
        CHECK(opened.tracks()[0].instrument == Instrument::Pad);
        REQUIRE(opened.tracks()[0].regions.size() == 1);
        const auto& opened_notes = notes_of(opened, 0, 0);
        REQUIRE(opened_notes.size() == 2);
        CHECK(opened_notes[1].start == 11000);
        CHECK(opened_notes[1].pitch == 64);
        CHECK(opened_notes[1].length == 2000);
        CHECK(opened_notes[0].velocity == doctest::Approx(0.8f));
    }
    std::filesystem::remove(file);

    // Undo goes back through the instrument, the join and the cut.
    REQUIRE(session.undo());
    CHECK(session.tracks()[0].instrument == Instrument::Pluck);
    REQUIRE(session.undo());
    CHECK(session.tracks()[0].regions.size() == 1);
    CHECK(notes_of(session, 0, 0).size() == 2);
}

TEST_CASE("Exports include the instruments") {
    Session session;
    session.add_instrument_track("Bass", Instrument::Keys, 0, kRate);
    REQUIRE(session.set_region_notes(0, 0, {make_note(0, kRate / 2, 45)}));
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "toh_test_notes.wav";
    std::string error;
    REQUIRE_MESSAGE(session.export_wav(file, toh::audio::SampleFormat::Int16, &error), error);
    CHECK(std::filesystem::file_size(file) > 44 + 2 * 2 * kRate / 2);
    std::filesystem::remove(file);
}

TEST_CASE("Live notes sound while stopped and end after their release") {
    Session session;
    session.add_instrument_track("Live", Instrument::Organ, 0, kRate);
    session.set_live_track(0);
    REQUIRE(session.note_on(69, 0.8f));
    std::vector<float> out(2 * 2000);
    session.engine().render(out.data(), 2000);
    float loudest = 0.0f;
    for (const float sample : out) {
        loudest = std::max(loudest, std::fabs(sample));
    }
    CHECK(loudest > 0.05f);
    CHECK_FALSE(session.is_playing());

    session.note_off(69);
    std::vector<float> tail(2 * 6000);
    session.engine().render(tail.data(), 6000);  // longer than the organ's release
    session.engine().render(out.data(), 2000);
    for (const float sample : out) {
        CHECK(sample == 0.0f);
    }
}

TEST_CASE("Notes played while recording become a note region") {
    Session session;
    session.add_instrument_track("Lead", Instrument::Keys, 0, 2 * kRate);
    session.set_live_track(0);
    CHECK(session.is_instrument_track(0));
    REQUIRE(session.start_recording(0));
    CHECK(session.is_recording());
    std::vector<float> out(2 * 4800);
    session.engine().render(out.data(), 4800);
    session.update();
    REQUIRE(session.note_on(60, 0.7f));
    session.engine().render(out.data(), 4800);
    session.engine().render(out.data(), 4800);
    session.note_off(60);
    session.engine().render(out.data(), 4800);
    session.stop_recording();
    session.update();
    CHECK_FALSE(session.is_recording());

    REQUIRE(session.tracks()[0].regions.size() == 2);
    const auto& region = session.tracks()[0].regions[1];
    REQUIRE(region.has_notes());
    CHECK(region.start == 4800);
    const auto& notes = region.source->clip->notes->notes;
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].start == 0);
    CHECK(notes[0].length == 9600);
    CHECK(notes[0].pitch == 60);
    CHECK(notes[0].velocity == doctest::Approx(0.7f));
}
