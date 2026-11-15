#include "toh/app/library.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>

#include "toh/app/session.h"

namespace toh::app {
namespace {

// General MIDI drum notes.
constexpr int kKick = 36;
constexpr int kSnare = 38;
constexpr int kClap = 39;
constexpr int kClosedHat = 42;
constexpr int kOpenHat = 46;
constexpr int kCrash = 49;
constexpr int kLowTom = 45;
constexpr int kMidTom = 48;
constexpr int kHighTom = 50;

constexpr int kDrums = 128;
constexpr int kPiano = 0;
constexpr int kElectricPiano = 4;
constexpr int kFingerBass = 33;
constexpr int kWarmPad = 89;

void hit(PartInfo& part, double beat, int pitch, float velocity = 0.8f, double length = 0.25) {
    part.notes.push_back(PartNote{beat, length, pitch, velocity});
}

// Closed hi-hats on every eighth note, louder on the beats.
void eighth_hats(PartInfo& part) {
    for (int k = 0; k < part.bars * 8; ++k) {
        hit(part, k * 0.5, kClosedHat, k % 2 == 0 ? 0.75f : 0.5f, 0.125);
    }
}

PartInfo drums(const char* name, int bars) {
    PartInfo part;
    part.category = "Drums";
    part.name = name;
    part.program = kDrums;
    part.bars = bars;
    return part;
}

// A chord held for a whole bar, for every chord in `chords`.
PartInfo chords(const char* category, const char* name, int program,
                std::initializer_list<std::initializer_list<int>> chords) {
    PartInfo part;
    part.category = category;
    part.name = name;
    part.program = program;
    part.bars = static_cast<int>(chords.size());
    int bar = 0;
    for (const auto& chord : chords) {
        for (const int pitch : chord) {
            part.notes.push_back(PartNote{bar * 4.0, 3.9, pitch, 0.7f});
        }
        ++bar;
    }
    return part;
}

// Eighth notes on the root of each bar's chord.
PartInfo root_bass(const char* name, std::initializer_list<int> roots, bool octaves) {
    PartInfo part;
    part.category = "Bass";
    part.name = name;
    part.program = kFingerBass;
    part.bars = static_cast<int>(roots.size());
    int bar = 0;
    for (const int root : roots) {
        for (int k = 0; k < 8; ++k) {
            const int pitch = octaves && k % 2 == 1 ? root + 12 : root;
            part.notes.push_back(PartNote{bar * 4.0 + k * 0.5, 0.45, pitch, k % 2 == 0 ? 0.85f : 0.7f});
        }
        ++bar;
    }
    return part;
}

// Sixteenth notes going up and down through the notes of a chord.
PartInfo arpeggio(const char* name, std::initializer_list<int> chord, int bars) {
    PartInfo part;
    part.category = "Melodies";
    part.name = name;
    part.program = kElectricPiano;
    part.bars = bars;
    // Up through the chord and back down, without repeating the top and the bottom note.
    const std::vector<int> notes(chord);
    std::vector<int> cycle = notes;
    for (std::size_t k = notes.size() - 1; k-- > 1;) {
        cycle.push_back(notes[k]);
    }
    for (int k = 0; k < bars * 16; ++k) {
        const int pitch = cycle[static_cast<std::size_t>(k) % cycle.size()];
        part.notes.push_back(PartNote{k * 0.25, 0.22, pitch, k % 4 == 0 ? 0.8f : 0.6f});
    }
    return part;
}

std::vector<PartInfo> make_parts() {
    std::vector<PartInfo> parts;

    PartInfo rock = drums("Rock beat", 2);
    eighth_hats(rock);
    for (int bar = 0; bar < 2; ++bar) {
        const double b = bar * 4.0;
        hit(rock, b + 0.0, kKick, 0.95f);
        hit(rock, b + 2.0, kKick, 0.9f);
        hit(rock, b + 2.5, kKick, 0.8f);
        hit(rock, b + 1.0, kSnare, 0.9f);
        hit(rock, b + 3.0, kSnare, 0.9f);
    }
    parts.push_back(rock);

    PartInfo four = drums("Four on the floor", 2);
    for (int beat = 0; beat < 8; ++beat) {
        hit(four, beat, kKick, 0.95f);
        hit(four, beat + 0.5, kOpenHat, 0.6f);
        if (beat % 2 == 1) {
            hit(four, beat, kClap, 0.85f);
        }
    }
    parts.push_back(four);

    PartInfo hiphop = drums("Hip-hop beat", 2);
    eighth_hats(hiphop);
    for (int bar = 0; bar < 2; ++bar) {
        const double b = bar * 4.0;
        hit(hiphop, b + 0.0, kKick, 0.95f);
        hit(hiphop, b + 0.75, kKick, 0.7f);
        hit(hiphop, b + 2.5, kKick, 0.85f);
        hit(hiphop, b + 1.0, kSnare, 0.9f);
        hit(hiphop, b + 3.0, kSnare, 0.9f);
    }
    parts.push_back(hiphop);

    PartInfo half = drums("Half-time beat", 2);
    eighth_hats(half);
    for (int bar = 0; bar < 2; ++bar) {
        const double b = bar * 4.0;
        hit(half, b + 0.0, kKick, 0.95f);
        hit(half, b + 1.5, kKick, 0.75f);
        hit(half, b + 2.0, kSnare, 0.95f);
    }
    parts.push_back(half);

    PartInfo breakbeat = drums("Breakbeat", 2);
    eighth_hats(breakbeat);
    for (int bar = 0; bar < 2; ++bar) {
        const double b = bar * 4.0;
        hit(breakbeat, b + 0.0, kKick, 0.95f);
        hit(breakbeat, b + 0.5, kKick, 0.8f);
        hit(breakbeat, b + 2.5, kKick, 0.85f);
        hit(breakbeat, b + 1.0, kSnare, 0.9f);
        hit(breakbeat, b + 1.75, kSnare, 0.5f);
        hit(breakbeat, b + 3.25, kSnare, 0.55f);
        hit(breakbeat, b + 3.0, kSnare, 0.9f);
    }
    parts.push_back(breakbeat);

    PartInfo fill = drums("Drum fill", 1);
    for (int k = 0; k < 16; ++k) {
        const int pitch = k < 4 ? kSnare : (k < 8 ? kHighTom : (k < 12 ? kMidTom : kLowTom));
        hit(fill, k * 0.25, pitch, 0.6f + 0.02f * static_cast<float>(k), 0.2);
    }
    hit(fill, 3.75, kCrash, 0.9f, 0.25);
    parts.push_back(fill);

    parts.push_back(root_bass("Root notes (C)", {36, 36}, false));
    parts.push_back(root_bass("Octaves (C)", {36, 36}, true));
    parts.push_back(root_bass("Bass line (Am-F-C-G)", {33, 29, 36, 31}, false));
    {
        PartInfo walking;
        walking.category = "Bass";
        walking.name = "Walking bass (C-F)";
        walking.program = kFingerBass;
        walking.bars = 2;
        int beat = 0;
        for (const int pitch : {36, 40, 43, 45, 41, 45, 48, 47}) {
            walking.notes.push_back(PartNote{static_cast<double>(beat++), 0.9, pitch, 0.8f});
        }
        parts.push_back(walking);
    }

    parts.push_back(chords("Chords", "C-G-Am-F (piano)", kPiano,
                           {{60, 64, 67}, {55, 59, 62}, {57, 60, 64}, {53, 57, 60}}));
    parts.push_back(chords("Chords", "Am-F-C-G (piano)", kPiano,
                           {{57, 60, 64}, {53, 57, 60}, {55, 60, 64}, {55, 59, 62}}));
    parts.push_back(chords("Chords", "Dm-G-C (piano)", kPiano, {{50, 53, 57}, {55, 59, 62}, {52, 55, 60}, {52, 55, 60}}));
    parts.push_back(chords("Chords", "C-G-Am-F (pad)", kWarmPad,
                           {{48, 60, 64, 67}, {43, 55, 59, 62}, {45, 57, 60, 64}, {41, 53, 57, 60}}));

    parts.push_back(arpeggio("Arpeggio (C major)", {60, 64, 67, 72}, 2));
    parts.push_back(arpeggio("Arpeggio (A minor)", {57, 60, 64, 69}, 2));
    return parts;
}

}  // namespace

const std::vector<PartInfo>& library_parts() {
    static const std::vector<PartInfo> parts = make_parts();
    return parts;
}

std::vector<core::Note> part_notes(const Session& session, const PartInfo& part, std::int64_t start,
                                   std::int64_t* length) {
    const double first = session.beats_at(std::max<std::int64_t>(start, 0));
    const auto frame_of = [&session, first, start](double beat) {
        return static_cast<std::int64_t>(std::llround(session.frame_at(first + beat))) - start;
    };
    std::vector<core::Note> notes;
    notes.reserve(part.notes.size());
    for (const PartNote& note : part.notes) {
        core::Note out;
        out.start = std::max<std::int64_t>(frame_of(note.beat), 0);
        out.length = std::max<std::int64_t>(frame_of(note.beat + note.length) - out.start, 1);
        out.pitch = static_cast<std::uint8_t>(std::clamp(note.pitch, 0, 127));
        out.velocity = note.velocity;
        notes.push_back(out);
    }
    std::sort(notes.begin(), notes.end(), [](const core::Note& a, const core::Note& b) { return a.start < b.start; });
    if (length != nullptr) {
        *length = std::max<std::int64_t>(frame_of(part.bars * 4.0), 1);
    }
    return notes;
}

}  // namespace toh::app
