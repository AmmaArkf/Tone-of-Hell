// Tests for reading and writing Standard MIDI Files.

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "doctest.h"
#include "toh/app/midi_file.h"
#include "toh/app/session.h"
#include "toh/core/soundfont.h"

using toh::app::MidiSong;
using toh::app::Session;
using toh::core::Note;

namespace {

Note make_note(std::int64_t start, std::int64_t length, int pitch, float velocity = 0.8f) {
    Note note;
    note.start = start;
    note.length = length;
    note.pitch = static_cast<std::uint8_t>(pitch);
    note.velocity = velocity;
    return note;
}

}  // namespace

TEST_CASE("A MIDI file with running status and note-on as note-off is read") {
    // Format 0, 96 ticks per quarter note, 60 BPM: one tick is 500 frames at 48 kHz.
    const std::vector<unsigned char> bytes = {
        'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0, 96,
        'M', 'T', 'r', 'k', 0, 0, 0, 30,
        0x00, 0xFF, 0x51, 0x03, 0x0F, 0x42, 0x40,  // tempo: 1 000 000 microseconds per quarter
        0x00, 0xC0, 0x05,                          // program 5 on channel 1
        0x60, 0x90, 0x3C, 0x64,                    // after one quarter: C4, velocity 100
        0x00, 0x3E, 0x50,                          // running status: D4, velocity 80
        0x30, 0x3C, 0x00,                          // half a quarter later: C4 off (velocity 0)
        0x00, 0x3E, 0x00,                          // D4 off
        0x00, 0xFF, 0x2F, 0x00};
    MidiSong song;
    std::string error;
    REQUIRE_MESSAGE(toh::app::parse_midi(bytes, 48000, song, &error), error);
    CHECK(song.bpm == doctest::Approx(60.0));
    REQUIRE(song.parts.size() == 1);
    const auto& part = song.parts[0];
    CHECK(part.program == 5);
    REQUIRE(part.notes.size() == 2);
    CHECK(part.notes[0].start == 48000);
    CHECK(part.notes[0].length == 24000);
    CHECK(part.notes[0].pitch == 60);
    CHECK(part.notes[0].velocity == doctest::Approx(100.0f / 127.0f));
    CHECK(part.notes[1].pitch == 62);
    CHECK(part.notes[1].start == 48000);
    CHECK(song.frames == 72000);

    CHECK_FALSE(toh::app::parse_midi({'n', 'o', 't', ' ', 'm', 'i', 'd', 'i'}, 48000, song, &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("Projects are exported to MIDI and imported again") {
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "toh_test.mid";
    std::string error;
    {
        Session session;
        toh::app::MetronomeSettings metronome = session.metronome();
        metronome.bpm = 120.0;
        metronome.beats_per_bar = 3;
        REQUIRE(session.set_metronome(metronome));
        // At 120 BPM and 480 ticks per quarter note, one tick is 50 frames.
        session.add_instrument_track("Piano", toh::core::Instrument::Keys, 0, 96000, 0);
        REQUIRE(session.set_region_notes(0, 0, {make_note(0, 24000, 60), make_note(24000, 12000, 64, 0.5f)}));
        session.add_instrument_track("Beat", toh::core::Instrument::Keys, 48000, 48000,
                                     toh::core::SoundFont::kDrums);
        REQUIRE(session.set_region_notes(1, 0, {make_note(1000, 5000, 36)}));
        session.add_empty_track("Audio, no notes");
        REQUIRE_MESSAGE(session.export_midi(file, &error), error);
    }

    Session imported;
    REQUIRE_MESSAGE(imported.import_midi(file, &error), error);
    CHECK(imported.metronome().beats_per_bar == 3);
    CHECK(imported.metronome().bpm == doctest::Approx(120.0));
    REQUIRE(imported.tracks().size() == 2);
    CHECK(imported.tracks()[0].name == "Piano");
    CHECK(imported.tracks()[0].program == 0);
    CHECK(imported.tracks()[1].program == toh::core::SoundFont::kDrums);
    const auto& piano = imported.tracks()[0].regions[0].source->clip->notes->notes;
    REQUIRE(piano.size() == 2);
    CHECK(piano[1].start == 24000);
    CHECK(piano[1].length == 12000);
    CHECK(piano[1].pitch == 64);
    CHECK(piano[1].velocity == doctest::Approx(0.5f).epsilon(0.02));
    const auto& drums = imported.tracks()[1].regions[0].source->clip->notes->notes;
    REQUIRE(drums.size() == 1);
    CHECK(drums[0].start == 49000);  // the region began at 48000
    std::filesystem::remove(file);

    Session empty;
    CHECK_FALSE(empty.export_midi(file, &error));
}
