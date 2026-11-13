#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include "toh/app/i18n.h"

namespace toh::app {

/// Things the user can do with a keyboard shortcut. Every user interface offers the same list.
enum class Action {
    PlayPause,
    Stop,
    BackToStart,
    Record,
    Reverse,
    Loop,
    Metronome,
    Split,
    RemoveRegion,
    JoinRegions,
    Deselect,
    AddFiles,
    ZoomIn,
    ZoomOut,
    ZoomFit,
    NewTrack,
    FullScreen,
    NewProject,
    OpenProject,
    SaveProject,
    SaveProjectAs,
    CutTool,
    Undo,
    Redo,
    Faster,
    Slower,
    NormalSpeed,
    Count
};

constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);

struct ActionInfo {
    Action action;
    const char* id;                ///< Name in the settings file, for example "play_pause".
    const char* name;              ///< English name shown to the user (pass it through tr()).
    const char* default_shortcut;  ///< For example "Space" or "Ctrl+O"; empty for none.
};

/// The description of an action. `action` must not be Action::Count.
const ActionInfo& action_info(Action action) noexcept;

/// What the program remembers between runs. Shortcuts are stored as text such as "Ctrl+O":
/// optional "Ctrl+", "Shift+" and "Alt+" followed by a key name; an empty text means none.
struct Settings {
    Settings();  // fills in the default shortcuts

    Language language = Language::English;
    std::string output_device;  ///< Name of the output device; empty for the system default.
    std::string input_device;   ///< Name of the input device; empty for the system default.
    float master_gain = 1.0f;
    float input_gain = 1.0f;
    /// Latency correction for recordings in frames; negative means "use the default".
    std::int64_t record_offset_frames = -1;
    /// Only the selected tracks play (when any track is selected).
    bool play_selected_only = false;
    /// Dragged regions and edges stick to the edges of other regions.
    bool magnet = true;
    /// The reverse button starts playback when it is stopped or paused.
    bool reverse_starts_playback = true;
    /// Reverse switches itself off when backwards playback reaches the start.
    bool reverse_off_at_start = true;
    /// How the window opens: 0 as a normal window, 1 maximized, 2 full screen.
    int start_window = 0;
    /// Where the save and open dialogs for projects start (UTF-8). Empty: the folder
    /// "Tone of Hell" in the user's Documents folder.
    std::string projects_folder;
    /// Write a log file for every session.
    bool write_logs = true;
    /// With the metronome on, playback goes on past the end of the project.
    bool metronome_endless = true;
    std::array<std::string, kActionCount> shortcuts;

    const std::string& shortcut(Action action) const noexcept {
        return shortcuts[static_cast<std::size_t>(action)];
    }
    void reset_shortcuts();

    /// Reads a settings file. Unknown lines are ignored and missing values keep their
    /// defaults, so files from older and newer versions both load. Returns false if the file
    /// cannot be opened.
    bool load(const std::filesystem::path& path);
    /// Writes the settings file. Returns false if it cannot be written.
    bool save(const std::filesystem::path& path) const;
};

}  // namespace toh::app
