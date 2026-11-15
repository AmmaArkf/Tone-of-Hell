#pragma once

#include <cstdint>
#include <vector>

#include "toh/core/notes.h"

namespace toh::app {

class Session;

/// One note of a ready-made part, in beats from the part's beginning.
struct PartNote {
    double beat = 0.0;
    double length = 0.25;
    int pitch = 60;
    float velocity = 0.8f;
};

/// A ready-made part of the library: a drum rhythm, a bass line, chords. Written for 4/4 time.
struct PartInfo {
    const char* category = "";  ///< English, for example "Drums"
    const char* name = "";       ///< English, for example "Rock beat"
    int program = 0;             ///< General MIDI program (128 = drums)
    int bars = 1;
    std::vector<PartNote> notes;
};

/// The ready-made parts, in the order they are shown.
const std::vector<PartInfo>& library_parts();

/// The notes of a part placed at `start`, timed by the tempo of the session there (so they
/// follow tempo changes). `length` receives the length of the part in frames.
std::vector<core::Note> part_notes(const Session& session, const PartInfo& part, std::int64_t start,
                                   std::int64_t* length);

}  // namespace toh::app
