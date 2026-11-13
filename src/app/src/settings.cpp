#include "toh/app/settings.h"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace toh::app {
namespace {

const ActionInfo kActions[kActionCount] = {
    {Action::PlayPause, "play_pause", "Play or pause", "Space"},
    {Action::Stop, "stop", "Stop", ""},
    {Action::BackToStart, "back_to_start", "Back to the start", "Home"},
    {Action::Record, "record", "Start or stop recording", "R"},
    {Action::Reverse, "reverse", "Play backwards or forwards", ""},
    {Action::Loop, "loop", "Loop on or off", "L"},
    {Action::Metronome, "metronome", "Metronome on or off", "K"},
    {Action::Split, "split", "Split at the playhead", "S"},
    {Action::RemoveRegion, "remove_region", "Remove the selected region", "Delete"},
    {Action::JoinRegions, "join_regions", "Join with the next region", "J"},
    {Action::Deselect, "deselect", "Deselect", "Escape"},
    {Action::AddFiles, "add_files", "Add audio files", "Ctrl+O"},
    {Action::ZoomIn, "zoom_in", "Zoom in", ""},
    {Action::ZoomOut, "zoom_out", "Zoom out", ""},
    {Action::ZoomFit, "zoom_fit", "Show the whole project", "F"},
    {Action::NewTrack, "new_track", "New track", "Ctrl+T"},
    {Action::FullScreen, "full_screen", "Full screen on or off", "F11"},
};

std::string trimmed(const std::string& text) {
    const char* const blanks = " \t\r\n";
    const std::size_t first = text.find_first_not_of(blanks);
    if (first == std::string::npos) {
        return std::string();
    }
    return text.substr(first, text.find_last_not_of(blanks) - first + 1);
}

// Reads a number written with a decimal point, whatever the system's language settings are.
bool parse_float(const std::string& text, float& value) {
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    float parsed = 0.0f;
    if (!(stream >> parsed)) {
        return false;
    }
    value = parsed;
    return true;
}

}  // namespace

const ActionInfo& action_info(Action action) noexcept {
    return kActions[static_cast<std::size_t>(action)];
}

Settings::Settings() { reset_shortcuts(); }

void Settings::reset_shortcuts() {
    for (std::size_t i = 0; i < kActionCount; ++i) {
        shortcuts[i] = kActions[i].default_shortcut;
    }
}

bool Settings::load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        const std::size_t equals = line.find('=');
        if (equals == std::string::npos || line.empty() || line[0] == '#') {
            continue;
        }
        const std::string key = trimmed(line.substr(0, equals));
        const std::string value = trimmed(line.substr(equals + 1));

        if (key == "language") {
            language = value == "ru" ? Language::Russian : Language::English;
        } else if (key == "output_device") {
            output_device = value;
        } else if (key == "input_device") {
            input_device = value;
        } else if (key == "master_gain") {
            float number = 0.0f;
            if (parse_float(value, number) && number >= 0.0f && number <= 8.0f) {
                master_gain = number;
            }
        } else if (key == "input_gain") {
            float number = 0.0f;
            if (parse_float(value, number) && number >= 0.0f && number <= 8.0f) {
                input_gain = number;
            }
        } else if (key == "record_offset_frames") {
            char* end = nullptr;
            const long long number = std::strtoll(value.c_str(), &end, 10);
            if (end != value.c_str()) {
                record_offset_frames = number;
            }
        } else if (key == "start_window") {
            const int number = std::atoi(value.c_str());
            if (number >= 0 && number <= 2) {
                start_window = number;
            }
        } else if (key == "reverse_starts_playback") {
            reverse_starts_playback = value == "1" || value == "true";
        } else if (key == "reverse_off_at_start") {
            reverse_off_at_start = value == "1" || value == "true";
        } else if (key == "magnet") {
            magnet = value == "1" || value == "true";
        } else if (key == "play_selected_only") {
            play_selected_only = value == "1" || value == "true";
        } else if (key.rfind("shortcut.", 0) == 0) {
            const std::string id = key.substr(9);
            for (std::size_t i = 0; i < kActionCount; ++i) {
                if (id == kActions[i].id) {
                    shortcuts[i] = value;
                }
            }
        }
    }
    return true;
}

bool Settings::save(const std::filesystem::path& path) const {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return false;
    }
    file.imbue(std::locale::classic());
    file << "# Tone of Hell settings. Edit them in the program: Edit > Settings.\n";
    file << "language=" << (language == Language::Russian ? "ru" : "en") << "\n";
    file << "output_device=" << output_device << "\n";
    file << "input_device=" << input_device << "\n";
    file << "master_gain=" << master_gain << "\n";
    file << "input_gain=" << input_gain << "\n";
    file << "record_offset_frames=" << record_offset_frames << "\n";
    file << "play_selected_only=" << (play_selected_only ? 1 : 0) << "\n";
    file << "magnet=" << (magnet ? 1 : 0) << "\n";
    file << "start_window=" << start_window << "\n";
    file << "reverse_starts_playback=" << (reverse_starts_playback ? 1 : 0) << "\n";
    file << "reverse_off_at_start=" << (reverse_off_at_start ? 1 : 0) << "\n";
    for (std::size_t i = 0; i < kActionCount; ++i) {
        file << "shortcut." << kActions[i].id << "=" << shortcuts[i] << "\n";
    }
    return static_cast<bool>(file);
}

}  // namespace toh::app
