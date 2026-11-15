#pragma once

#include <cstdint>
#include <vector>

namespace toh::core {

/// The built-in instruments that play note regions. They are original, simple synthesizers.
enum class Instrument : std::uint8_t {
    Keys,   ///< An electric piano: a bright attack that settles.
    Organ,  ///< Steady drawbar-like tones.
    Pad,    ///< Slow, soft and wide.
    Pluck,  ///< A short plucked string.
    // Synthesizers of level 3. They are computed from the time alone like the ones above, so
    // their filters are spectra that move with envelopes (no state, real-time safe).
    FmKeys,         ///< A frequency-modulated electric piano with a bell-like tine.
    FmBell,         ///< An inharmonic FM bell that rings on.
    AnalogBass,     ///< A saw wave through a low-pass filter that closes after the attack, plus a sub octave.
    Supersaw,       ///< Five detuned saw waves, wide and bright.
    WavetableLead,  ///< A lead that morphs from a dark saw to a hollow square, with vibrato.
    SynthBrass,     ///< A saw whose filter swells open, like a brass section.
};
constexpr int kInstrumentCount = 10;

/// One note. `start` and `length` are in frames of the note list (0 = its first frame).
struct Note {
    std::int64_t start = 0;
    std::int64_t length = 0;
    std::uint8_t pitch = 60;  ///< MIDI note number: 60 = middle C, 69 = A 440 Hz.
    float velocity = 0.8f;    ///< 0 to 1.
};

/// The notes of a note region, sorted by start. `frames` is the length of the list.
struct NoteData {
    std::vector<Note> notes;
    std::int64_t frames = 0;
};

/// How long an instrument keeps sounding after a note ends.
double instrument_release_seconds(Instrument instrument) noexcept;

/// The sound of one note `t_frames` frames after it started (also after it ended, during the
/// release). It depends only on that time, so it stays right through seeks, loops, reverse
/// playback and exports. Real-time safe.
float note_sample(Instrument instrument, const Note& note, double t_frames,
                  double sample_rate) noexcept;

}  // namespace toh::core
