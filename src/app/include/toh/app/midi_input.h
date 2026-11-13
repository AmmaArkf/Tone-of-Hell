#pragma once

#include <memory>
#include <string>
#include <vector>

namespace toh::app {

/// A key pressed or released on a MIDI keyboard.
struct MidiEvent {
    bool on = true;
    int pitch = 60;
    float velocity = 0.8f;  // 0 to 1
};

/// Notes from the MIDI keyboards connected to the computer (Windows: the system's MIDI inputs;
/// Linux: the raw MIDI devices in /dev/snd). Events arrive on a thread of the system and wait
/// in a queue until take() collects them.
class MidiInput {
public:
    MidiInput();
    ~MidiInput();
    MidiInput(const MidiInput&) = delete;
    MidiInput& operator=(const MidiInput&) = delete;

    /// Opens every MIDI input that can be opened (closing those open before).
    void open_all();
    void close_all();
    /// Names of the open inputs.
    const std::vector<std::string>& devices() const noexcept;
    /// Moves the events received since the last call to `events`.
    void take(std::vector<MidiEvent>& events);

    struct Impl;  // the system part, defined in midi_input.cpp

private:
    std::unique_ptr<Impl> impl_;
};

}  // namespace toh::app
