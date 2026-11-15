#pragma once

#include <string>
#include <vector>

#include "toh/app/settings.h"

namespace toh::app {

class Session;

/// What a knob, fader or button of a MIDI controller is assigned to.
enum class ControlTarget {
    MasterVolume,  ///< 0..127 gives 0..200 %
    TrackVolume,   ///< of track `index`, 0..200 %
    TrackPan,      ///< of track `index`, left to right
    TrackMute,     ///< a button: every press switches it
    TrackSolo,     ///< a button
    GroupVolume,   ///< of group `index`, 0..200 %
    Action,        ///< a button that does Action `index`, like its keyboard shortcut
};

/// One control of a MIDI controller (a control change number on a channel) and its target.
struct ControlMapping {
    int channel = 0;     ///< 0..15
    int controller = 0;  ///< 0..127
    ControlTarget target = ControlTarget::MasterVolume;
    int index = 0;       ///< the track, the group or the Action
    float last = -1.0f;  ///< the last value seen (buttons react when it goes from below 0.5 to above)
};

/// Turns the control changes of MIDI controllers into changes of the session ("MIDI learn":
/// choose a target, then move the control that should change it).
class Controllers {
public:
    const std::vector<ControlMapping>& mappings() const noexcept { return mappings_; }
    /// The next control that moves is assigned to the target (it loses any other target, and
    /// the target loses any other control).
    void learn(ControlTarget target, int index);
    void cancel_learning() noexcept { learning_ = false; }
    bool learning() const noexcept { return learning_; }
    bool learning(ControlTarget target, int index) const noexcept {
        return learning_ && learn_target_ == target && learn_index_ == index;
    }
    /// The control assigned to a target, or null.
    const ControlMapping* find(ControlTarget target, int index) const noexcept;
    void forget(ControlTarget target, int index);

    /// A control moved to `value` (0..1). Changes the session, and adds to `actions` the actions
    /// of buttons that were pressed (a user interface performs them). Returns true when the
    /// control was learned just now.
    bool handle(Session& session, int channel, int controller, float value, std::vector<Action>& actions);

    /// For the settings file: one line "channel|controller|target|index" per control.
    std::vector<std::string> to_lines() const;
    void from_lines(const std::vector<std::string>& lines);

private:
    std::vector<ControlMapping> mappings_;
    bool learning_ = false;
    ControlTarget learn_target_ = ControlTarget::MasterVolume;
    int learn_index_ = 0;
};

/// "CC 7, channel 1" (in English; numbers counted from 1 for the channel).
std::string control_text(const ControlMapping& mapping);

}  // namespace toh::app
