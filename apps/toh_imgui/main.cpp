// toh_imgui: the Dear ImGui user interface of Tone of Hell.
//
// This file only draws the shared toh::app::Session and forwards user actions to it. It must
// not contain audio or project logic, so that other user interfaces stay equivalent.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "toh/app/build_info.h"
#include "toh/app/i18n.h"
#include "toh/app/log.h"
#include "toh/app/midi_input.h"
#include "toh/app/settings.h"
#include "toh/app/session.h"
#include "toh/core/clip.h"

namespace {

using toh::app::GridStep;
using toh::app::RegionInfo;
using toh::app::Session;
using toh::app::TrackInfo;
using toh::app::Action;
using toh::app::Settings;

// Translates a text of the user interface into the chosen language.
inline const char* T_(const char* english) { return toh::app::tr(english); }

// ---- Files waiting to be loaded ----------------------------------------------------------

// The file dialog may answer on another thread, so the list is protected by a mutex.
struct PendingFiles {
    std::mutex mutex;
    std::vector<std::string> paths;  // UTF-8

    void add(const char* path) {
        const std::lock_guard<std::mutex> lock(mutex);
        paths.emplace_back(path);
    }
    std::vector<std::string> take() {
        const std::lock_guard<std::mutex> lock(mutex);
        std::vector<std::string> result;
        result.swap(paths);
        return result;
    }
};

void wake_main_loop() {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_EVENT_USER;
    SDL_PushEvent(&event);
}

void SDLCALL file_dialog_callback(void* user_data, const char* const* file_list, int /*filter*/) {
    auto* pending = static_cast<PendingFiles*>(user_data);
    if (file_list == nullptr) {
        return;  // the dialog failed
    }
    for (const char* const* file = file_list; *file != nullptr; ++file) {
        pending->add(*file);
    }
    wake_main_loop();
}

// ---- State that belongs to this window only ----------------------------------------------

// A file name chosen in the "export" dialog, waiting to be written by the main loop.
struct PendingExport {
    std::mutex mutex;
    bool ready = false;
    std::string path;  // UTF-8
    toh::audio::SampleFormat format = toh::audio::SampleFormat::Int24;
    int mp3_kbps = -1;  // -1: a WAV file; 0 or more: an MP3 file (0 = variable bit rate)
};

void SDLCALL export_dialog_callback(void* user_data, const char* const* file_list, int /*filter*/) {
    auto* request = static_cast<PendingExport*>(user_data);
    if (file_list == nullptr || file_list[0] == nullptr) {
        return;  // the dialog failed or was cancelled
    }
    {
        const std::lock_guard<std::mutex> lock(request->mutex);
        request->path = file_list[0];
        request->ready = true;
    }
    wake_main_loop();
}

// A path chosen in a project or folder dialog, waiting for the main loop.
struct PendingPath {
    enum class Kind { SaveProject, OpenProject, ProjectsFolder, SoundFont, ImportMidi, ExportMidi };
    std::mutex mutex;
    bool ready = false;
    Kind kind = Kind::SaveProject;
    std::string path;  // UTF-8
};

void SDLCALL path_dialog_callback(void* user_data, const char* const* file_list, int /*filter*/) {
    auto* request = static_cast<PendingPath*>(user_data);
    {
        // An empty path means that the dialog failed or was cancelled.
        const std::lock_guard<std::mutex> lock(request->mutex);
        request->path = file_list != nullptr && file_list[0] != nullptr ? file_list[0] : "";
        request->ready = true;
    }
    wake_main_loop();
}

// A picture chosen in the "track picture" file dialog, waiting to be applied by the main loop.
struct PendingIcon {
    std::mutex mutex;
    bool ready = false;
    std::size_t track = 0;
    std::string path;  // UTF-8
};

void SDLCALL icon_dialog_callback(void* user_data, const char* const* file_list, int /*filter*/) {
    auto* request = static_cast<PendingIcon*>(user_data);
    if (file_list == nullptr || file_list[0] == nullptr) {
        return;  // the dialog failed or was cancelled
    }
    {
        const std::lock_guard<std::mutex> lock(request->mutex);
        request->path = file_list[0];
        request->ready = true;
    }
    wake_main_loop();
}

// Which part of the timeline is visible.
struct View {
    double pixels_per_second = 100.0;
    double start_seconds = 0.0;
    bool fit_pending = true;     // show the whole project as soon as there is something to show
    float lane_origin_x = 0.0f;  // screen position and width of the lanes, from the last frame
    float lane_width = 800.0f;
};

enum class DragKind { None, Seek, Move, TrimStart, TrimEnd, FadeIn, FadeOut, Select };

struct UiState {
    std::string error;
    std::uint64_t seen_clips = 0;  // clip count when the indicator was last reset
    std::vector<double> taps;      // times of the latest "Tap tempo" presses, in seconds
    View view;
    bool follow = true;            // keep the playhead in view while playing
    std::size_t known_tracks = 0;

    bool selected = false;         // the selected region, if any
    std::size_t selected_track = 0;
    std::size_t selected_region = 0;

    DragKind drag = DragKind::None;
    std::size_t drag_track = 0;    // the track of the dragged region (it can change while moving)
    std::size_t drag_owner = 0;    // the lane where the drag began: it handles the mouse
    std::vector<std::pair<float, float>> lane_spans;  // top and bottom of every lane on screen
    std::size_t drag_region = 0;
    std::int64_t drag_grab = 0;    // distance from the region start to the grabbed frame
    std::int64_t magnet_frame = -1;  // the edge a dragged region sticks to, or -1
    bool magnet_end = false;         // true when it is the end of the region that sticks
    float scroll_grab = 0.0f;      // where the timeline scrollbar thumb was grabbed

    bool quit = false;             // "Exit" was chosen in the menu
    std::string notice;            // a short message after something succeeded

    Settings settings;             // what is remembered between runs
    std::filesystem::path settings_path;
    bool settings_changed = false; // write the settings file soon
    PendingPath path_request;      // project and folder dialogs
    enum class Then { Nothing, Quit, NewProject, OpenProject };
    Then after_save = Then::Nothing;    // what to do once the "save as" dialog has saved
    Then pending_then = Then::Nothing;  // waits for the answer about unsaved changes
    bool ask_unsaved = false;
    bool cut_tool = false;              // the scissors: a click cuts a region
    // A selected part of the tracks (dragged out inside regions) and what was cut or copied.
    bool has_range = false;
    std::size_t range_first = 0;
    std::size_t range_last = 0;
    std::int64_t range_begin = 0;
    std::int64_t range_end = 0;
    std::int64_t range_anchor = 0;       // where the drag began
    std::size_t focus_track = 0;         // the lane clicked last: pasting goes there
    Session::Clipboard clipboard;
    toh::app::MidiInput midi;            // MIDI keyboards
    std::vector<toh::app::MidiEvent> midi_events;
    bool musical_keyboard = false;       // the letter keys play notes
    int keyboard_octave = 4;             // the A key plays C of this octave
    std::array<int, 16> key_pitch{};     // the note each letter key started (-1: none)

    // The piano roll: one note region, edited on a working copy of its notes.
    bool roll_open = false;
    std::size_t roll_track = 0;
    std::size_t roll_region = 0;
    const void* roll_source = nullptr;  // the notes the working copy was taken from
    std::vector<toh::core::Note> roll_notes;
    std::vector<toh::core::Note> roll_origin;  // the notes when the drag began
    std::vector<char> roll_selected;           // one flag per note of roll_notes
    int roll_mode = 0;  // 0 nothing, 1 moving, 2 length, 3 playhead, 4 listening, 5 selection box
    std::size_t roll_anchor = 0;               // the note the drag began on
    double roll_start_frame = 0.0;
    int roll_start_pitch = 0;
    std::int64_t roll_last_length = 0;         // new notes get the length used last
    float roll_beat_px = 48.0f;
    bool roll_scroll = false;                  // scroll to middle C once
    bool roll_focused = false;
    bool roll_hovered = false;                 // the canvas had the mouse in the last frame
    float roll_scroll_x = 0.0f;                // its scrolling and left edge in the last frame
    float roll_canvas_x = 0.0f;
    int roll_preview = -1;                     // the note sounding while a note is dragged
    float roll_box_y = 0.0f;                   // where a selection box began, inside the canvas
    std::vector<char> roll_box_base;           // the selection before the box (kept with Ctrl)
    std::filesystem::path logs_folder;
    std::filesystem::path recovery_file;  // where autosave writes
    bool recovery_written = false;        // the recovery file holds unsaved changes
    bool ask_recovery = false;            // offer to restore it (after a crash)
    std::uint64_t last_autosave_ticks = 0;
    std::uint64_t last_recovery_check = 0;
    std::string dropped_picture;  // a picture file dropped on the window (UTF-8)
    float dropped_y = 0.0f;       // where it was dropped
    std::string logged_error;           // the last error and notice written to the log
    std::string logged_notice;
    std::uint64_t logged_disk_misses = 0;
    std::uint64_t logged_dropped = 0;
    std::uint64_t last_log_ticks = 0;
    std::string window_title;      // what the window title shows now
    bool show_settings = false;
    int capturing = -1;            // the action whose shortcut is being changed, or -1
    toh::audio::DeviceList devices;
    bool devices_listed = false;
    bool open_track_menu = false;  // open the "add a track" menu in the track area

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    PendingIcon icon_request;
    std::map<std::string, SDL_Texture*> textures;  // track pictures from files; null if unreadable

    bool rename_open = false;  // open the rename box for rename_track
    std::size_t rename_track = 0;
    char rename_buffer[256] = {};
    bool armed = false;            // a track is chosen to record onto
    std::size_t armed_track = 0;
    bool show_help = false;
    bool show_about = false;
};

// ---- Small helpers -----------------------------------------------------------------------

std::string format_time(std::int64_t frames, std::uint32_t sample_rate) {
    const double seconds = static_cast<double>(frames) / static_cast<double>(sample_rate);
    const int minutes = static_cast<int>(seconds / 60.0);
    char text[32];
    std::snprintf(text, sizeof(text), "%d:%06.3f", minutes, seconds - minutes * 60.0);
    return text;
}

float frame_to_x(const Session& session, const View& view, std::int64_t frame) {
    const double seconds = static_cast<double>(frame) / static_cast<double>(session.sample_rate());
    return view.lane_origin_x +
           static_cast<float>((seconds - view.start_seconds) * view.pixels_per_second);
}

std::int64_t x_to_frame(const Session& session, const View& view, float x) {
    const double seconds =
        static_cast<double>(x - view.lane_origin_x) / view.pixels_per_second + view.start_seconds;
    return std::llround(seconds * static_cast<double>(session.sample_rate()));
}

void fit_view(const Session& session, View& view) {
    const double seconds = static_cast<double>(session.length_frames()) /
                           static_cast<double>(session.sample_rate());
    view.pixels_per_second = static_cast<double>(view.lane_width) / std::max(seconds * 1.05, 1.0);
    view.start_seconds = 0.0;
}

void zoom_view(View& view, double factor, float anchor_x) {
    const double offset = static_cast<double>(anchor_x - view.lane_origin_x);
    const double anchor_seconds = view.start_seconds + offset / view.pixels_per_second;
    view.pixels_per_second = std::clamp(view.pixels_per_second * factor, 0.5, 20000.0);
    view.start_seconds = std::max(0.0, anchor_seconds - offset / view.pixels_per_second);
}

// Maps a linear peak value to a meter position between 0 and 1 (-60 dB .. 0 dB).
float meter_fraction(float peak) {
    if (peak <= 0.001f) {
        return 0.0f;
    }
    const float decibels = 20.0f * std::log10(peak);
    return std::clamp((decibels + 60.0f) / 60.0f, 0.0f, 1.0f);
}

// A horizontal level meter from -60 dB to 0 dB, centred in a row of the given height. It has a
// frame and scale marks so that it reads as a meter even when it is empty.
void draw_meter(float peak, float width, float row_height) {
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, row_height));

    const float bar = std::max(5.0f, std::min(row_height, 0.55f * ImGui::GetFontSize()));
    const ImVec2 low(origin.x, origin.y + (row_height - bar) * 0.5f);
    const ImVec2 high(origin.x + width, low.y + bar);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(low, high, IM_COL32(14, 15, 17, 255), 2.0f);

    const float fraction = meter_fraction(peak);
    if (fraction > 0.0f) {
        const ImU32 colour = peak > 1.0f    ? IM_COL32(230, 64, 51, 255)
                             : peak > 0.5f  ? IM_COL32(230, 190, 51, 255)
                                            : IM_COL32(77, 190, 90, 255);
        draw_list->AddRectFilled(low, ImVec2(low.x + width * fraction, high.y), colour, 2.0f);
    }
    for (const float decibels : {-48.0f, -36.0f, -24.0f, -12.0f, -6.0f}) {
        const float x = low.x + width * (decibels + 60.0f) / 60.0f;
        draw_list->AddLine(ImVec2(x, low.y), ImVec2(x, high.y), IM_COL32(125, 128, 138, 150));
    }
    draw_list->AddRect(low, high, IM_COL32(95, 98, 108, 255), 2.0f);

    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        if (peak > 0.001f) {
            ImGui::SetTooltip(T_("Level meter: %.1f dB"), static_cast<double>(20.0f * std::log10(peak)));
        } else {
            ImGui::SetTooltip("%s", T_("Level meter (silent). The marks are at -48, -36, -24, -12 and -6 dB."));
        }
    }
}

// A button that stays highlighted while `active` is true. Returns true when clicked.
bool toggle_button(const char* label, bool active, const ImVec4& active_colour,
                   bool small = true) {
    if (active) {
        ImGui::PushStyleColor(ImGuiCol_Button, active_colour);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, active_colour);
    }
    const bool clicked = small ? ImGui::SmallButton(label) : ImGui::Button(label);
    if (active) {
        ImGui::PopStyleColor(2);
    }
    return clicked;
}

// The magnet button of the toolbar: a horseshoe magnet, coloured when it is on.
bool magnet_button(bool on) {
    const float size = ImGui::GetFrameHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("magnet", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImGuiStyle& style = ImGui::GetStyle();
    ImU32 background = ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    if (on) {
        background = hovered ? IM_COL32(70, 110, 180, 255) : IM_COL32(50, 90, 160, 255);
    }
    d->AddRectFilled(origin, ImVec2(origin.x + size, origin.y + size), background, style.FrameRounding);

    const float cx = origin.x + size * 0.5f;
    const float cy = origin.y + size * 0.5f;
    const float radius = size * 0.24f;
    const float thick = size * 0.16f;
    const float top = origin.y + size * 0.2f;
    const ImU32 body = on ? IM_COL32(225, 60, 60, 255) : IM_COL32(150, 150, 160, 255);
    const ImU32 tips = on ? IM_COL32(235, 235, 240, 255) : IM_COL32(190, 190, 200, 255);
    d->PathArcTo(ImVec2(cx, cy), radius, 0.0f, 3.14159265f, 16);
    d->PathStroke(body, ImDrawFlags_None, thick);
    d->AddLine(ImVec2(cx - radius, cy), ImVec2(cx - radius, top + thick), body, thick);
    d->AddLine(ImVec2(cx + radius, cy), ImVec2(cx + radius, top + thick), body, thick);
    d->AddRectFilled(ImVec2(cx - radius - thick * 0.5f, top), ImVec2(cx - radius + thick * 0.5f, top + thick), tips);
    d->AddRectFilled(ImVec2(cx + radius - thick * 0.5f, top), ImVec2(cx + radius + thick * 0.5f, top + thick), tips);
    if (!on) {
        d->AddLine(ImVec2(origin.x + size * 0.18f, origin.y + size * 0.82f),
                   ImVec2(origin.x + size * 0.82f, origin.y + size * 0.18f), IM_COL32(230, 90, 80, 230),
                   2.0f);
    }
    return clicked;
}

// The scissors button of the toolbar: a click on a region cuts it while it is on.
bool scissors_button(bool on) {
    const float size = ImGui::GetFrameHeight();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton("scissors", ImVec2(size, size));
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* d = ImGui::GetWindowDrawList();
    ImU32 background = ImGui::GetColorU32(hovered ? ImGuiCol_ButtonHovered : ImGuiCol_Button);
    if (on) {
        background = hovered ? IM_COL32(70, 110, 180, 255) : IM_COL32(50, 90, 160, 255);
    }
    d->AddRectFilled(origin, ImVec2(origin.x + size, origin.y + size), background,
                     ImGui::GetStyle().FrameRounding);
    const auto P = [&](float u, float v) { return ImVec2(origin.x + u * size, origin.y + v * size); };
    const ImU32 ink = on ? IM_COL32(245, 245, 250, 255) : IM_COL32(170, 170, 180, 255);
    d->AddLine(P(0.30f, 0.14f), P(0.64f, 0.62f), ink, 0.08f * size);
    d->AddLine(P(0.70f, 0.14f), P(0.36f, 0.62f), ink, 0.08f * size);
    d->AddCircle(P(0.30f, 0.74f), 0.12f * size, ink, 16, 0.07f * size);
    d->AddCircle(P(0.70f, 0.74f), 0.12f * size, ink, 16, 0.07f * size);
    return clicked;
}

// True when the last widget was double-clicked: sliders use it to return to their default.
bool double_clicked() {
    return ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);
}

void tooltip(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip("%s", text);
    }
}

// Shortens a UTF-8 text with "..." until it fits into the given width.
std::string fit_text(const std::string& text, float width) {
    if (ImGui::CalcTextSize(text.c_str()).x <= width) {
        return text;
    }
    std::string cut = text;
    while (!cut.empty()) {
        // Remove one whole character: its continuation bytes and then its first byte.
        while (!cut.empty()) {
            const auto byte = static_cast<unsigned char>(cut.back());
            cut.pop_back();
            if ((byte & 0xC0u) != 0x80u) {
                break;
            }
        }
        const std::string candidate = cut + "...";
        if (ImGui::CalcTextSize(candidate.c_str()).x <= width) {
            return candidate;
        }
    }
    return "...";
}

const RegionInfo* selected_region(const Session& session, const UiState& ui) {
    if (!ui.selected || ui.selected_track >= session.tracks().size() ||
        ui.selected_region >= session.tracks()[ui.selected_track].regions.size()) {
        return nullptr;
    }
    return &session.tracks()[ui.selected_track].regions[ui.selected_region];
}

// ---- Editing commands shared by keys and menus -------------------------------------------

// Cuts the selected region at the playhead; without a selection, every region under it.
void split_at_playhead(Session& session, const UiState& ui) {
    const std::int64_t frame = session.position_frames();
    if (selected_region(session, ui) != nullptr) {
        session.split_region(ui.selected_track, ui.selected_region, frame);
        return;
    }
    for (std::size_t t = 0; t < session.tracks().size(); ++t) {
        const std::size_t count = session.tracks()[t].regions.size();  // new parts are appended
        for (std::size_t r = 0; r < count; ++r) {
            session.split_region(t, r, frame);
        }
    }
}

// ---- Cut, copy and paste -----------------------------------------------------------------

bool has_range(const UiState& ui) { return ui.has_range && ui.range_end > ui.range_begin; }

Session::Range ui_range(const UiState& ui) {
    Session::Range range;
    range.first_track = ui.range_first;
    range.last_track = ui.range_last;
    range.begin = ui.range_begin;
    range.end = ui.range_end;
    return range;
}

// What cut and copy take: the selected part, else the selected region.
bool can_take(const Session& session, const UiState& ui) {
    return has_range(ui) || selected_region(session, ui) != nullptr;
}

void copy_selection(Session& session, UiState& ui) {
    if (has_range(ui)) {
        ui.clipboard = session.copy_range(ui_range(ui));
    } else if (const RegionInfo* region = selected_region(session, ui)) {
        ui.clipboard = Session::Clipboard();
        RegionInfo copy = *region;
        copy.start = 0;
        ui.clipboard.length = copy.length;
        ui.clipboard.tracks.push_back({copy});
    } else {
        return;
    }
    if (!ui.clipboard.empty()) {
        ui.notice = T_("Copied. Ctrl+V pastes it at the playhead.");
    }
}

void cut_selection(Session& session, UiState& ui) {
    if (has_range(ui)) {
        session.cut_range(ui_range(ui), &ui.clipboard);
        ui.has_range = false;
        ui.selected = false;
    } else if (selected_region(session, ui) != nullptr) {
        copy_selection(session, ui);
        session.remove_region(ui.selected_track, ui.selected_region);
        ui.selected = false;
    }
}

void delete_selection(Session& session, UiState& ui) {
    session.cut_range(ui_range(ui));
    ui.has_range = false;
    ui.selected = false;
}

void keep_selection(Session& session, UiState& ui) {
    if (has_range(ui)) {
        session.keep_range(ui_range(ui));
        ui.selected = false;
    }
}

// Pastes onto the lane clicked last, at the playhead, and moves the playhead after what was
// pasted, so that pasting again puts the next copy right behind it.
void paste_clipboard(Session& session, UiState& ui) {
    if (ui.clipboard.empty()) {
        return;
    }
    const std::size_t track = ui.focus_track < session.tracks().size() ? ui.focus_track
                                                                       : Session::kNewTrack;
    const std::int64_t frame = session.position_frames();
    const std::size_t first = std::min(track, session.tracks().size());
    if (session.paste(ui.clipboard, track, frame)) {
        ui.has_range = true;
        ui.range_first = first;
        ui.range_last = first + ui.clipboard.tracks.size() - 1;
        ui.range_begin = frame;
        ui.range_end = frame + ui.clipboard.length;
        ui.selected = false;
        session.seek_frames(frame + ui.clipboard.length);
    }
}

void remove_selected_region(Session& session, UiState& ui) {
    if (selected_region(session, ui) != nullptr) {
        session.remove_region(ui.selected_track, ui.selected_region);
        ui.selected = false;
    }
}

// The region of the same track that comes after the given one: the nearest one that starts
// later. Returns the number of regions when there is none.
std::size_t next_region(const TrackInfo& track, std::size_t index) {
    const std::int64_t start = track.regions[index].start;
    std::size_t best = track.regions.size();
    for (std::size_t r = 0; r < track.regions.size(); ++r) {
        const std::int64_t other = track.regions[r].start;
        if (r == index || other < start || (other == start && r < index)) {
            continue;
        }
        if (best == track.regions.size() || other < track.regions[best].start) {
            best = r;
        }
    }
    return best;
}

bool can_join_next(const Session& session, const UiState& ui) {
    if (selected_region(session, ui) == nullptr) {
        return false;
    }
    const TrackInfo& track = session.tracks()[ui.selected_track];
    return next_region(track, ui.selected_region) < track.regions.size();
}

void join_with_next(Session& session, UiState& ui) {
    if (!can_join_next(session, ui)) {
        return;
    }
    const std::size_t next = next_region(session.tracks()[ui.selected_track], ui.selected_region);
    std::size_t joined = 0;
    if (session.join_regions(ui.selected_track, {ui.selected_region, next}, &joined)) {
        ui.selected_region = joined;
    }
}

// Glues every region of the track with the selected region into one.
void join_selected_track(Session& session, UiState& ui) {
    if (selected_region(session, ui) == nullptr) {
        return;
    }
    std::vector<std::size_t> all(session.tracks()[ui.selected_track].regions.size());
    for (std::size_t r = 0; r < all.size(); ++r) {
        all[r] = r;
    }
    std::size_t joined = 0;
    if (session.join_regions(ui.selected_track, all, &joined)) {
        ui.selected_region = joined;
    }
}

bool selected_track_has_regions_to_join(const Session& session, const UiState& ui) {
    return selected_region(session, ui) != nullptr &&
           session.tracks()[ui.selected_track].regions.size() >= 2;
}

void set_play_selected_only(Session& session, UiState& ui, bool enabled) {
    session.set_play_selected_only(enabled);
    ui.settings.play_selected_only = enabled;
    ui.settings_changed = true;
}

// A click on a track header: selects that track only; with Ctrl or Shift, adds it to the
// selection or takes it out. Clicking the only selected track deselects it.
void click_track(Session& session, std::size_t index) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool was_selected = session.tracks()[index].selected;
    if (io.KeyCtrl || io.KeyShift) {
        session.set_track_selected(index, !was_selected);
        return;
    }
    std::size_t count = 0;
    for (const TrackInfo& track : session.tracks()) {
        count += track.selected ? 1 : 0;
    }
    if (was_selected && count == 1) {
        session.clear_track_selection();
    } else {
        session.select_only_track(index);
    }
}

// ---- Menu bar ----------------------------------------------------------------------------

// Asks for a file name to export to. `mp3_kbps` is -1 for a WAV file in `format`, otherwise the
// MP3 bit rate (0 = variable).
void open_export_dialog(PendingExport& exports, SDL_Window* window,
                        toh::audio::SampleFormat format, int mp3_kbps = -1) {
    {
        const std::lock_guard<std::mutex> lock(exports.mutex);
        exports.format = format;
        exports.mp3_kbps = mp3_kbps;
        exports.ready = false;
    }
    static const SDL_DialogFileFilter wav_filters[] = {{"WAV audio", "wav"}};
    static const SDL_DialogFileFilter mp3_filters[] = {{"MP3 audio", "mp3"}};
    SDL_ShowSaveFileDialog(&export_dialog_callback, &exports, window,
                           mp3_kbps >= 0 ? mp3_filters : wav_filters, 1, nullptr);
}

// The record button: stops a running recording, otherwise opens the input if needed and starts
// one on the armed track (or on a new track).
void toggle_recording(Session& session, UiState& ui) {
    if (session.is_recording()) {
        session.stop_recording();
        return;
    }
    const std::size_t track = ui.armed && ui.armed_track < session.tracks().size()
                                  ? ui.armed_track
                                  : Session::kNewTrack;
    // An instrument track records notes from the keyboards; only sound needs the input.
    if (!session.is_instrument_track(track) && !session.input_enabled()) {
        std::string error;
        if (!session.set_input_enabled(true, &error)) {
            ui.error = "Recording: " + error;
            return;
        }
    }
    if (!session.start_recording(track)) {
        ui.error = "Recording could not be started.";
    }
}

void draw_record_settings(Session& session, UiState& ui) {
    const float font = ImGui::GetFontSize();
    ImGui::TextUnformatted(T_("Recording"));
    ImGui::Separator();

    bool input = session.input_enabled();
    if (ImGui::Checkbox(T_("Input device open"), &input)) {
        std::string error;
        if (!session.set_input_enabled(input, &error) && input) {
            ui.error = "Input: " + error;
        }
    }
    ImGui::TextDisabled("%s", T_("It is opened automatically when you press Record."));

    bool monitor = session.monitoring();
    if (ImGui::Checkbox(T_("Monitor: hear the input"), &monitor)) {
        session.set_monitoring(monitor);
    }
    ImGui::TextDisabled("%s", T_("Use headphones: with loudspeakers the microphone hears itself."));

    const double rate = static_cast<double>(session.sample_rate());
    auto milliseconds =
        static_cast<float>(static_cast<double>(session.record_offset_frames()) * 1000.0 / rate);
    ImGui::SetNextItemWidth(8.0f * font);
    if (ImGui::InputFloat(T_("Latency correction, ms"), &milliseconds, 1.0f, 10.0f, "%.1f")) {
        session.set_record_offset_frames(
            std::llround(static_cast<double>(milliseconds) * rate / 1000.0));
    }
    ImGui::TextDisabled("%s",
                        T_("A take is placed this much earlier, to make up for the delay of the\n"
                           "sound card. Raise it if recordings sound late, lower it if early."));
}

void open_file_dialog(PendingFiles& pending, SDL_Window* window) {
    static const SDL_DialogFileFilter filters[] = {{"Audio files", "wav;flac;mp3"},
                                                   {"All files", "*"}};
    SDL_ShowOpenFileDialog(&file_dialog_callback, &pending, window, filters, 2, nullptr, true);
}

// ---- Shortcuts ---------------------------------------------------------------------------

// A key with its modifiers. Shortcuts are stored as text such as "Ctrl+O" (see Settings).
struct Chord {
    ImGuiKey key = ImGuiKey_None;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
};

Chord parse_chord(const std::string& text) {
    Chord chord;
    std::string rest = text;
    for (;;) {
        if (rest.rfind("Ctrl+", 0) == 0) {
            chord.ctrl = true;
            rest.erase(0, 5);
        } else if (rest.rfind("Shift+", 0) == 0) {
            chord.shift = true;
            rest.erase(0, 6);
        } else if (rest.rfind("Alt+", 0) == 0) {
            chord.alt = true;
            rest.erase(0, 4);
        } else {
            break;
        }
    }
    if (rest.empty()) {
        return Chord();
    }
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const auto key = static_cast<ImGuiKey>(k);
        if (rest == ImGui::GetKeyName(key)) {
            chord.key = key;
            return chord;
        }
    }
    return Chord();  // an unknown key name: no shortcut
}

std::string chord_text(const Chord& chord) {
    if (chord.key == ImGuiKey_None) {
        return std::string();
    }
    std::string text;
    if (chord.ctrl) {
        text += "Ctrl+";
    }
    if (chord.shift) {
        text += "Shift+";
    }
    if (chord.alt) {
        text += "Alt+";
    }
    return text + ImGui::GetKeyName(chord.key);
}

bool chord_pressed(const Chord& chord) {
    if (chord.key == ImGuiKey_None) {
        return false;
    }
    const ImGuiIO& io = ImGui::GetIO();
    return io.KeyCtrl == chord.ctrl && io.KeyShift == chord.shift && io.KeyAlt == chord.alt &&
           ImGui::IsKeyPressed(chord.key, false);
}

// The shortcut of an action as text for menus, or null when it has none.
const char* key_label(const UiState& ui, Action action) {
    const std::string& text = ui.settings.shortcut(action);
    return text.empty() ? nullptr : text.c_str();
}

// Waits for a key to be pressed (for the settings window). Modifier keys alone, mouse buttons
// and game controllers do not count.
bool capture_chord(Chord& chord) {
    const ImGuiIO& io = ImGui::GetIO();
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
        const auto key = static_cast<ImGuiKey>(k);
        const std::string name = ImGui::GetKeyName(key);
        if (name.rfind("Mouse", 0) == 0 || name.rfind("Gamepad", 0) == 0 ||
            name.find("Ctrl") != std::string::npos || name.find("Shift") != std::string::npos ||
            name.find("Alt") != std::string::npos || name.find("Super") != std::string::npos ||
            name.find("Mod") != std::string::npos) {
            continue;
        }
        if (ImGui::IsKeyPressed(key, false)) {
            chord.key = key;
            chord.ctrl = io.KeyCtrl;
            chord.shift = io.KeyShift;
            chord.alt = io.KeyAlt;
            return true;
        }
    }
    return false;
}

// Adds an empty track; with `arm` it is also chosen for the next recording.
void add_new_track(Session& session, UiState& ui, bool arm) {
    char name[64];
    std::snprintf(name, sizeof(name), T_("Track %d"), static_cast<int>(session.tracks().size() + 1));
    const std::size_t index = session.add_empty_track(name);
    if (index >= session.tracks().size()) {
        ui.error = T_("The track limit is reached.");
        return;
    }
    if (arm) {
        ui.armed = true;
        ui.armed_track = index;
    }
}

// ---- Track pictures ----------------------------------------------------------------------

namespace icons {

constexpr ImU32 kInk = IM_COL32(245, 245, 250, 255);
constexpr ImU32 kShade = IM_COL32(0, 0, 0, 90);
constexpr float kPi = 3.14159265f;

// Maps (0..1, 0..1) inside the picture's square to the screen.
struct Frame {
    ImVec2 origin;
    float size;
    ImVec2 operator()(float u, float v) const {
        return ImVec2(origin.x + u * size, origin.y + v * size);
    }
};

void voice(ImDrawList* d, ImVec2 o, float s) {
    const Frame P{o, s};
    d->AddRectFilled(P(0.38f, 0.12f), P(0.62f, 0.52f), kInk, 0.12f * s);
    d->PathArcTo(P(0.5f, 0.42f), 0.2f * s, 0.0f, kPi, 16);
    d->PathStroke(kInk, ImDrawFlags_None, 0.05f * s);
    d->AddLine(P(0.5f, 0.62f), P(0.5f, 0.8f), kInk, 0.05f * s);
    d->AddLine(P(0.34f, 0.82f), P(0.66f, 0.82f), kInk, 0.05f * s);
}

void guitar(ImDrawList* d, ImVec2 o, float s) {
    const Frame P{o, s};
    d->AddLine(P(0.48f, 0.5f), P(0.8f, 0.14f), kInk, 0.07f * s);
    d->AddRectFilled(P(0.76f, 0.06f), P(0.9f, 0.18f), kInk, 0.03f * s);
    d->AddCircleFilled(P(0.36f, 0.68f), 0.2f * s, kInk);
    d->AddCircleFilled(P(0.5f, 0.52f), 0.14f * s, kInk);
    d->AddCircleFilled(P(0.42f, 0.6f), 0.055f * s, kShade);
}

void bass(ImDrawList* d, ImVec2 o, float s) {
    const Frame P{o, s};
    d->AddLine(P(0.46f, 0.56f), P(0.86f, 0.1f), kInk, 0.06f * s);
    d->AddRectFilled(P(0.82f, 0.03f), P(0.94f, 0.14f), kInk, 0.03f * s);
    d->AddEllipseFilled(P(0.34f, 0.7f), ImVec2(0.17f * s, 0.15f * s), kInk);
    d->AddEllipseFilled(P(0.46f, 0.56f), ImVec2(0.12f * s, 0.1f * s), kInk);
    d->AddLine(P(0.27f, 0.64f), P(0.39f, 0.76f), kShade, 0.04f * s);
}

void drums(ImDrawList* d, ImVec2 o, float s) {
    const Frame P{o, s};
    d->AddRectFilled(P(0.2f, 0.5f), P(0.8f, 0.78f), kInk);
    d->AddEllipseFilled(P(0.5f, 0.78f), ImVec2(0.3f * s, 0.08f * s), kInk);
    d->AddEllipseFilled(P(0.5f, 0.5f), ImVec2(0.3f * s, 0.08f * s), kInk);
    d->AddEllipse(P(0.5f, 0.5f), ImVec2(0.3f * s, 0.08f * s), kShade, 0.0f, 0, 0.03f * s);
    for (const float u : {0.3f, 0.5f, 0.7f}) {
        d->AddLine(P(u, 0.6f), P(u, 0.82f), kShade, 0.025f * s);
    }
    d->AddLine(P(0.22f, 0.12f), P(0.56f, 0.44f), kInk, 0.045f * s);
    d->AddLine(P(0.78f, 0.12f), P(0.44f, 0.44f), kInk, 0.045f * s);
}

void keys(ImDrawList* d, ImVec2 o, float s) {
    const Frame P{o, s};
    const float left = 0.12f;
    const float right = 0.88f;
    const float top = 0.28f;
    const float bottom = 0.74f;
    const float key = (right - left) / 7.0f;
    d->AddRectFilled(P(left, top), P(right, bottom), kInk, 0.03f * s);
    for (int k = 1; k < 7; ++k) {
        const float x = left + key * static_cast<float>(k);
        d->AddLine(P(x, top), P(x, bottom), kShade, 0.02f * s);
    }
    for (const int k : {1, 2, 4, 5, 6}) {
        const float x = left + key * static_cast<float>(k);
        d->AddRectFilled(P(x - key * 0.3f, top), P(x + key * 0.3f, top + (bottom - top) * 0.6f),
                         IM_COL32(30, 30, 36, 255));
    }
}

void synth(ImDrawList* d, ImVec2 o, float s) {
    const Frame P{o, s};
    d->AddRect(P(0.1f, 0.24f), P(0.9f, 0.8f), kInk, 0.06f * s, 0, 0.045f * s);
    ImVec2 wave[25];
    for (int k = 0; k < 25; ++k) {
        const float t = static_cast<float>(k) / 24.0f;
        wave[k] = P(0.18f + 0.64f * t, 0.44f - 0.1f * std::sin(t * 3.0f * kPi));
    }
    d->AddPolyline(wave, 25, kInk, ImDrawFlags_None, 0.04f * s);
    for (const float u : {0.3f, 0.5f, 0.7f}) {
        d->AddCircleFilled(P(u, 0.66f), 0.045f * s, kInk);
    }
}

void strings(ImDrawList* d, ImVec2 o, float s) {
    const Frame P{o, s};
    d->AddLine(P(0.5f, 0.32f), P(0.5f, 0.1f), kInk, 0.05f * s);
    d->AddCircleFilled(P(0.5f, 0.08f), 0.04f * s, kInk);
    d->AddEllipseFilled(P(0.5f, 0.42f), ImVec2(0.13f * s, 0.11f * s), kInk);
    d->AddEllipseFilled(P(0.5f, 0.66f), ImVec2(0.17f * s, 0.15f * s), kInk);
    d->AddLine(P(0.44f, 0.56f), P(0.44f, 0.68f), kShade, 0.02f * s);
    d->AddLine(P(0.56f, 0.56f), P(0.56f, 0.68f), kShade, 0.02f * s);
    d->AddLine(P(0.12f, 0.86f), P(0.88f, 0.3f), IM_COL32(40, 30, 25, 230), 0.03f * s);
}

void brass(ImDrawList* d, ImVec2 o, float s) {
    const Frame P{o, s};
    d->AddRectFilled(P(0.08f, 0.47f), P(0.14f, 0.57f), kInk);
    d->AddLine(P(0.12f, 0.52f), P(0.62f, 0.52f), kInk, 0.06f * s);
    d->AddLine(P(0.24f, 0.52f), P(0.24f, 0.66f), kInk, 0.05f * s);
    d->AddLine(P(0.24f, 0.66f), P(0.6f, 0.66f), kInk, 0.05f * s);
    d->AddTriangleFilled(P(0.58f, 0.52f), P(0.9f, 0.3f), P(0.9f, 0.74f), kInk);
    for (const float u : {0.34f, 0.42f, 0.5f}) {
        d->AddRectFilled(P(u - 0.025f, 0.34f), P(u + 0.025f, 0.5f), kInk);
    }
}

void sample(ImDrawList* d, ImVec2 o, float s) {
    const Frame P{o, s};
    static const float heights[] = {0.1f, 0.25f, 0.38f, 0.3f, 0.15f, 0.34f, 0.27f, 0.12f, 0.2f, 0.08f};
    for (int k = 0; k < 10; ++k) {
        const float u = 0.16f + 0.075f * static_cast<float>(k);
        d->AddLine(P(u, 0.5f - heights[k]), P(u, 0.5f + heights[k]), kInk, 0.045f * s);
    }
}

void other(ImDrawList* d, ImVec2 o, float s) {
    const Frame P{o, s};
    d->AddEllipseFilled(P(0.4f, 0.72f), ImVec2(0.13f * s, 0.09f * s), kInk, -0.4f);
    d->AddLine(P(0.52f, 0.7f), P(0.52f, 0.16f), kInk, 0.05f * s);
    d->AddBezierCubic(P(0.52f, 0.16f), P(0.62f, 0.24f), P(0.72f, 0.3f), P(0.66f, 0.46f), kInk,
                      0.05f * s);
}

struct Builtin {
    const char* id;    // stored in TrackInfo::icon
    const char* name;  // shown to the user (translated)
    ImU32 colour;      // background
    void (*draw)(ImDrawList*, ImVec2, float);
};

const Builtin kBuiltins[] = {
    {"voice", "Voice", IM_COL32(170, 60, 90, 255), &voice},
    {"guitar", "Guitar", IM_COL32(200, 110, 40, 255), &guitar},
    {"bass", "Bass", IM_COL32(150, 50, 40, 255), &bass},
    {"drums", "Drums", IM_COL32(60, 110, 170, 255), &drums},
    {"keys", "Keys", IM_COL32(90, 70, 150, 255), &keys},
    {"synth", "Synth", IM_COL32(40, 140, 130, 255), &synth},
    {"strings", "Strings", IM_COL32(140, 90, 50, 255), &strings},
    {"brass", "Brass", IM_COL32(180, 150, 40, 255), &brass},
    {"sample", "Sample", IM_COL32(70, 130, 70, 255), &sample},
    {"other", "Other", IM_COL32(90, 95, 110, 255), &other},
};

const Builtin* find(const std::string& id) {
    for (const Builtin& builtin : kBuiltins) {
        if (id == builtin.id) {
            return &builtin;
        }
    }
    return nullptr;
}

}  // namespace icons

// The texture of a picture file, loaded once. Null when the file cannot be read.
SDL_Texture* texture_for(UiState& ui, const std::string& path) {
    const auto found = ui.textures.find(path);
    if (found != ui.textures.end()) {
        return found->second;
    }
    SDL_Texture* texture = nullptr;
    if (ui.renderer != nullptr) {
        if (SDL_Surface* surface = SDL_LoadSurface(path.c_str())) {  // PNG or BMP, UTF-8 path
            texture = SDL_CreateTextureFromSurface(ui.renderer, surface);
            SDL_DestroySurface(surface);
        }
        if (texture == nullptr && std::filesystem::exists(std::filesystem::u8path(path))) {
            ui.error = std::string(T_("Cannot show the picture ")) + path + ": " + SDL_GetError();
        }
    }
    ui.textures[path] = texture;
    return texture;
}

// Draws a track picture into a square: a built-in drawing, an image file, or an empty frame.
void draw_track_icon(UiState& ui, const std::string& icon, ImVec2 origin, float size, bool hovered) {
    ImDrawList* d = ImGui::GetWindowDrawList();
    const ImVec2 corner(origin.x + size, origin.y + size);
    const float rounding = 0.14f * size;
    const icons::Builtin* builtin = icons::find(icon);
    d->AddRectFilled(origin, corner, builtin != nullptr ? builtin->colour : IM_COL32(44, 47, 54, 255),
                     rounding);

    if (builtin != nullptr) {
        builtin->draw(d, origin, size);
    } else if (icon.rfind("file:", 0) == 0) {
        SDL_Texture* texture = texture_for(ui, icon.substr(5));
        float width = 0.0f;
        float height = 0.0f;
        if (texture != nullptr && SDL_GetTextureSize(texture, &width, &height) && width > 0.0f &&
            height > 0.0f) {
            const float scale = std::min(size / width, size / height);
            const ImVec2 middle(origin.x + size * 0.5f, origin.y + size * 0.5f);
            const ImVec2 half(width * scale * 0.5f, height * scale * 0.5f);
            d->AddImage(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(texture)),
                        ImVec2(middle.x - half.x, middle.y - half.y),
                        ImVec2(middle.x + half.x, middle.y + half.y));
        } else {
            const ImVec2 text = ImGui::CalcTextSize("?");
            d->AddText(ImVec2(origin.x + (size - text.x) * 0.5f, origin.y + (size - text.y) * 0.5f),
                       IM_COL32(220, 120, 110, 255), "?");
        }
    } else {
        // No picture yet: a faint "+" invites to choose one.
        const icons::Frame P{origin, size};
        d->AddLine(P(0.5f, 0.38f), P(0.5f, 0.62f), IM_COL32(150, 155, 170, 140), 0.04f * size);
        d->AddLine(P(0.38f, 0.5f), P(0.62f, 0.5f), IM_COL32(150, 155, 170, 140), 0.04f * size);
    }
    if (hovered) {
        d->AddRect(origin, corner, IM_COL32(220, 230, 255, 220), rounding, 0, 2.0f);
    }
}

void open_icon_dialog(UiState& ui, std::size_t track) {
    {
        const std::lock_guard<std::mutex> lock(ui.icon_request.mutex);
        ui.icon_request.track = track;
        ui.icon_request.ready = false;
    }
    static const SDL_DialogFileFilter filters[] = {{"Images", "png;bmp"}};
    SDL_ShowOpenFileDialog(&icon_dialog_callback, &ui.icon_request, ui.window, filters, 1, nullptr,
                           false);
}

// The popup that chooses the picture of a track.
void draw_icon_popup(Session& session, UiState& ui, std::size_t track) {
    if (!ImGui::BeginPopup("track icon")) {
        return;
    }
    ImGui::TextUnformatted(T_("Track picture"));
    const float size = 3.0f * ImGui::GetFontSize();
    int column = 0;
    for (const icons::Builtin& builtin : icons::kBuiltins) {
        ImGui::PushID(builtin.id);
        if (column % 5 != 0) {
            ImGui::SameLine();
        }
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##picture", ImVec2(size, size))) {
            session.set_track_icon(track, builtin.id);
            ImGui::CloseCurrentPopup();
        }
        const bool hovered = ImGui::IsItemHovered();
        tooltip(T_(builtin.name));
        draw_track_icon(ui, builtin.id, origin, size, hovered);
        ImGui::PopID();
        ++column;
    }
    ImGui::Separator();
    if (ImGui::Button(T_("Choose an image..."))) {
        open_icon_dialog(ui, track);
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("No picture"))) {
        session.set_track_icon(track, "");
        ImGui::CloseCurrentPopup();
    }
    ImGui::TextDisabled("%s", T_("PNG or BMP. The picture is scaled to fit."));
    ImGui::EndPopup();
}

// ---- Instruments -------------------------------------------------------------------------

const char* instrument_name(toh::core::Instrument instrument) {
    static const char* const names[] = {"Keys", "Organ", "Pad", "Pluck"};
    return names[static_cast<int>(instrument)];
}

// The name of a program of the SoundFont, or of the built-in instrument.
std::string sound_name(const Session& session, int program, toh::core::Instrument instrument) {
    if (program >= 0 && session.soundfont()) {
        if (const auto* preset = session.soundfont()->find(program)) {
            return preset->name;
        }
    }
    return T_(instrument_name(instrument));
}

// A new track with an instrument and an empty note region of 8 bars (or the project's length).
void add_instrument_track(Session& session, UiState& ui, toh::core::Instrument instrument,
                          int program) {
    const std::int64_t length =
        std::max<std::int64_t>(session.length_frames(), std::llround(session.bar_frames() * 8.0));
    const std::size_t index = session.add_instrument_track(sound_name(session, program, instrument),
                                                           instrument, 0, length, program);
    if (index >= session.tracks().size()) {
        ui.error = T_("The track limit is reached.");
    }
}

// The menu of sounds: the instruments of the SoundFont by family, the drums, and the simple
// synthesizers. Returns true when one was chosen.
bool instrument_menu(const Session& session, int current_program,
                     toh::core::Instrument current_instrument, int& program,
                     toh::core::Instrument& instrument) {
    static const char* const families[] = {
        "Pianos", "Chromatic percussion", "Organs", "Guitars", "Basses", "Strings",
        "Ensembles", "Brass", "Reeds", "Pipes", "Synth leads", "Synth pads",
        "Synth effects", "Ethnic", "Percussive", "Sound effects"};
    bool chosen = false;
    const auto& font = session.soundfont();
    if (font) {
        for (int family = 0; family < 16; ++family) {
            if (!ImGui::BeginMenu(T_(families[family]))) {
                continue;
            }
            for (int p = family * 8; p < family * 8 + 8; ++p) {
                const auto* preset = font->find(p);
                if (preset == nullptr) {
                    continue;
                }
                const std::string label = preset->name + "##program" + std::to_string(p);
                if (ImGui::MenuItem(label.c_str(), nullptr, current_program == p)) {
                    program = p;
                    instrument = current_instrument;
                    chosen = true;
                }
            }
            ImGui::EndMenu();
        }
        if (font->find(toh::core::SoundFont::kDrums) != nullptr &&
            ImGui::MenuItem(T_("Drums"), nullptr, current_program == toh::core::SoundFont::kDrums)) {
            program = toh::core::SoundFont::kDrums;
            instrument = current_instrument;
            chosen = true;
        }
        ImGui::Separator();
    } else {
        ImGui::TextDisabled("%s", T_("No SoundFont: only the simple synthesizers"));
    }
    if (ImGui::BeginMenu(T_("Simple synthesizers"))) {
        for (int k = 0; k < toh::core::kInstrumentCount; ++k) {
            const auto choice = static_cast<toh::core::Instrument>(k);
            if (ImGui::MenuItem(T_(instrument_name(choice)), nullptr,
                                current_program < 0 && current_instrument == choice)) {
                program = -1;
                instrument = choice;
                chosen = true;
            }
        }
        ImGui::EndMenu();
    }
    return chosen;
}

std::filesystem::path default_soundfont_path() {
    const char* base = SDL_GetBasePath();
    return base != nullptr ? std::filesystem::u8path(base) / "sounds" / "FluidR3_GM.sf2"
                           : std::filesystem::path();
}

void load_soundfont(Session& session, UiState& ui, const std::filesystem::path& path) {
    std::error_code code;
    if (path.empty() || !std::filesystem::exists(path, code)) {
        toh::app::logging::warning("No SoundFont at " + path.u8string() +
                                   ": instrument tracks use the simple synthesizers");
        return;
    }
    std::string error;
    auto font = toh::core::SoundFont::load(path, &error);
    if (!font) {
        ui.error = std::string(T_("Instrument sounds: ")) + error;
        return;
    }
    toh::app::logging::info("SoundFont " + path.u8string() + ": " +
                            std::to_string(font->presets().size()) + " presets");
    session.set_soundfont(std::move(font));
}

// ---- Playback speed ----------------------------------------------------------------------

// The speed slider and choices, for menus and popups.
void draw_speed_items(Session& session, UiState& ui) {
    const double speed = session.speed();
    float percent = static_cast<float>(speed * 100.0);
    ImGui::SetNextItemWidth(14.0f * ImGui::GetFontSize());
    if (ImGui::SliderFloat("##speed", &percent, 25.0f, 400.0f, T_("Speed: %.0f%%"),
                           ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
        session.set_speed(std::round(percent) / 100.0);
    }
    if (double_clicked()) {
        session.set_speed(1.0);
    }
    tooltip(T_("Double-click: back to 100%"));
    ImGui::Separator();
    static const int presets[] = {50, 75, 100, 125, 150, 200};
    for (const int preset : presets) {
        char label[32];
        std::snprintf(label, sizeof(label), "%d%%", preset);
        if (ImGui::MenuItem(label, nullptr, std::lround(speed * 100.0) == preset)) {
            session.set_speed(preset / 100.0);
        }
    }
    ImGui::Separator();
    if (ImGui::MenuItem(T_("Play faster"), key_label(ui, Action::Faster))) {
        session.faster();
    }
    if (ImGui::MenuItem(T_("Play slower"), key_label(ui, Action::Slower))) {
        session.slower();
    }
    if (ImGui::MenuItem(T_("Normal speed"), key_label(ui, Action::NormalSpeed), false, speed != 1.0)) {
        session.set_speed(1.0);
    }
    ImGui::TextDisabled("%s", T_("The pitch changes with the speed, like on a tape."));
}

// "Playback" in the right-click menus: speed, play or pause, reverse.
void draw_playback_menu(Session& session, UiState& ui) {
    ImGui::Separator();
    if (ImGui::BeginMenu(T_("Playback"))) {
        if (ImGui::BeginMenu(T_("Speed"))) {
            draw_speed_items(session, ui);
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(T_(session.is_playing() ? "Pause" : "Play"), key_label(ui, Action::PlayPause))) {
            session.toggle_play();
        }
        if (ImGui::MenuItem(T_("Reverse"), key_label(ui, Action::Reverse), session.reverse())) {
            session.toggle_reverse();
        }
        ImGui::EndMenu();
    }
}

// ---- Projects ----------------------------------------------------------------------------

// The folder where project dialogs start: the one from the settings, or "Tone of Hell" in the
// user's Documents folder (created when needed).
std::filesystem::path projects_folder(const UiState& ui) {
    if (!ui.settings.projects_folder.empty()) {
        return std::filesystem::u8path(ui.settings.projects_folder);
    }
    std::filesystem::path folder;
    if (const char* documents = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS)) {
        folder = std::filesystem::u8path(documents) / "Tone of Hell";
    } else if (char* base = SDL_GetPrefPath("", "Tone of Hell")) {
        folder = std::filesystem::u8path(base) / "Projects";
        SDL_free(base);
    }
    std::error_code code;
    std::filesystem::create_directories(folder, code);
    return folder;
}

void open_path_dialog(Session& session, UiState& ui, PendingPath::Kind kind) {
    {
        const std::lock_guard<std::mutex> lock(ui.path_request.mutex);
        ui.path_request.kind = kind;
        ui.path_request.ready = false;
    }
    static const SDL_DialogFileFilter filters[] = {{"Tone of Hell project", "tohp"}};
    std::filesystem::path start = projects_folder(ui);
    if (kind == PendingPath::Kind::SaveProject) {
        const std::filesystem::path& current = session.project_file();
        start = current.empty() ? start / "Project.tohp" : current;
    }
    const std::string location = start.u8string();
    if (kind == PendingPath::Kind::ProjectsFolder) {
        SDL_ShowOpenFolderDialog(&path_dialog_callback, &ui.path_request, ui.window,
                                 location.c_str(), false);
    } else if (kind == PendingPath::Kind::SaveProject) {
        SDL_ShowSaveFileDialog(&path_dialog_callback, &ui.path_request, ui.window, filters, 1,
                               location.c_str());
    } else if (kind == PendingPath::Kind::SoundFont) {
        static const SDL_DialogFileFilter sf_filters[] = {{"SoundFont", "sf2"}};
        SDL_ShowOpenFileDialog(&path_dialog_callback, &ui.path_request, ui.window, sf_filters, 1,
                               nullptr, false);
    } else if (kind == PendingPath::Kind::ImportMidi) {
        static const SDL_DialogFileFilter midi_filters[] = {{"MIDI file", "mid;midi"}};
        SDL_ShowOpenFileDialog(&path_dialog_callback, &ui.path_request, ui.window, midi_filters, 1,
                               nullptr, false);
    } else if (kind == PendingPath::Kind::ExportMidi) {
        static const SDL_DialogFileFilter midi_filters[] = {{"MIDI file", "mid"}};
        const std::string midi_location = (projects_folder(ui) / "Song.mid").u8string();
        SDL_ShowSaveFileDialog(&path_dialog_callback, &ui.path_request, ui.window, midi_filters, 1,
                               midi_location.c_str());
    } else {
        SDL_ShowOpenFileDialog(&path_dialog_callback, &ui.path_request, ui.window, filters, 1,
                               location.c_str(), false);
    }
}

// Ctrl+S: saves to the project's file, or asks for one the first time (then it returns false;
// the save happens when the dialog answers).
bool save_project(Session& session, UiState& ui) {
    if (session.project_file().empty()) {
        open_path_dialog(session, ui, PendingPath::Kind::SaveProject);
        return false;
    }
    std::string error;
    if (session.save_project(session.project_file(), &error)) {
        ui.notice = std::string(T_("Saved: ")) + session.project_file().u8string();
        return true;
    }
    ui.error = std::string(T_("Save: ")) + error;
    return false;
}

// Opens a folder in the file manager.
void open_folder(const std::filesystem::path& folder) {
    std::error_code code;
    std::filesystem::create_directories(folder, code);
    const std::string url = "file:///" + folder.generic_u8string();
    SDL_OpenURL(url.c_str());
}

void do_then(Session& session, UiState& ui, UiState::Then then) {
    switch (then) {
        case UiState::Then::Quit:
            ui.quit = true;
            break;
        case UiState::Then::NewProject:
            session.new_project();
            ui.selected = false;
            ui.armed = false;
            toh::app::logging::info("New project");
            break;
        case UiState::Then::OpenProject:
            open_path_dialog(session, ui, PendingPath::Kind::OpenProject);
            break;
        case UiState::Then::Nothing:
            break;
    }
}

// Does `then` at once when nothing would be lost; otherwise asks about the changes first.
void unless_unsaved(Session& session, UiState& ui, UiState::Then then) {
    if (session.modified()) {
        ui.pending_then = then;
        ui.ask_unsaved = true;
    } else {
        do_then(session, ui, then);
    }
}

// Applies a path that a project or folder dialog returned.
void handle_path_request(Session& session, UiState& ui) {
    PendingPath::Kind kind;
    std::string path;
    {
        const std::lock_guard<std::mutex> lock(ui.path_request.mutex);
        if (!ui.path_request.ready) {
            return;
        }
        ui.path_request.ready = false;
        kind = ui.path_request.kind;
        path = ui.path_request.path;
    }
    if (path.empty()) {  // cancelled
        if (kind == PendingPath::Kind::SaveProject) {
            ui.after_save = UiState::Then::Nothing;
        }
        return;
    }
    std::string error;
    if (kind == PendingPath::Kind::SoundFont) {
        ui.settings.soundfont_path = path;
        ui.settings_changed = true;
        load_soundfont(session, ui, std::filesystem::u8path(path));
        return;
    }
    if (kind == PendingPath::Kind::ImportMidi) {
        if (session.import_midi(std::filesystem::u8path(path), &error)) {
            ui.notice = std::string(T_("Imported: ")) + path;
            fit_view(session, ui.view);
        } else {
            ui.error = std::string(T_("MIDI: ")) + error;
        }
        return;
    }
    if (kind == PendingPath::Kind::ExportMidi) {
        std::filesystem::path file = std::filesystem::u8path(path);
        if (file.extension() != ".mid" && file.extension() != ".midi") {
            file += ".mid";
        }
        if (session.export_midi(file, &error)) {
            ui.notice = std::string(T_("Exported: ")) + file.u8string();
        } else {
            ui.error = std::string(T_("MIDI: ")) + error;
        }
        return;
    }
    if (kind == PendingPath::Kind::ProjectsFolder) {
        ui.settings.projects_folder = path;
        ui.settings_changed = true;
    } else if (kind == PendingPath::Kind::SaveProject) {
        std::filesystem::path file = std::filesystem::u8path(path);
        if (file.extension() != ".tohp") {
            file += ".tohp";
        }
        // Every project gets a folder of its own, named like the project.
        if (file.parent_path().filename() != file.stem()) {
            const std::filesystem::path folder = file.parent_path() / file.stem();
            std::error_code code;
            std::filesystem::create_directories(folder, code);
            if (!code) {
                file = folder / file.filename();
            }
        }
        const UiState::Then then = ui.after_save;
        ui.after_save = UiState::Then::Nothing;
        if (session.save_project(file, &error)) {
            ui.notice = std::string(T_("Saved: ")) + file.u8string();
            do_then(session, ui, then);
        } else {
            ui.error = std::string(T_("Save: ")) + error;
        }
    } else if (session.open_project(std::filesystem::u8path(path), &error)) {
        ui.selected = false;
        ui.armed = false;
        fit_view(session, ui.view);
        ui.notice = std::string(T_("Opened: ")) + path;
    } else {
        ui.error = std::string(T_("Open: ")) + error;
    }
}

// The question about unsaved changes, before closing, starting anew or opening a project.
void draw_unsaved_popup(Session& session, UiState& ui) {
    if (ui.ask_unsaved) {
        ImGui::OpenPopup("###unsaved");
        ui.ask_unsaved = false;
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    const std::string title = std::string(T_("Unsaved changes")) + "###unsaved";
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        const std::string name = session.project_file().empty()
                                     ? std::string(T_("Untitled"))
                                     : session.project_file().stem().u8string();
        ImGui::Text(T_("Save the changes to \"%s\"?"), name.c_str());
        ImGui::TextDisabled("%s", T_("Without saving, the changes are lost."));
        ImGui::Spacing();
        if (ImGui::Button(T_("Save"))) {
            const UiState::Then then = ui.pending_then;
            ui.pending_then = UiState::Then::Nothing;
            ImGui::CloseCurrentPopup();
            if (session.project_file().empty()) {
                ui.after_save = then;  // the "save as" dialog finishes the job
                open_path_dialog(session, ui, PendingPath::Kind::SaveProject);
            } else if (save_project(session, ui)) {
                do_then(session, ui, then);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button(T_("Don't save"))) {
            const UiState::Then then = ui.pending_then;
            ui.pending_then = UiState::Then::Nothing;
            ImGui::CloseCurrentPopup();
            do_then(session, ui, then);
        }
        ImGui::SameLine();
        if (ImGui::Button(T_("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
            ui.pending_then = UiState::Then::Nothing;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// Writes the errors and notices the user saw, and audio trouble, to the session log.
void log_events(Session& session, UiState& ui) {
    if (!ui.error.empty() && ui.error != ui.logged_error) {
        toh::app::logging::error(ui.error);
        ui.logged_error = ui.error;
    }
    if (!ui.notice.empty() && ui.notice != ui.logged_notice) {
        toh::app::logging::info(ui.notice);
        ui.logged_notice = ui.notice;
    }
    const std::uint64_t now = SDL_GetTicks();
    if (now - ui.last_log_ticks < 2000) {
        return;  // audio trouble is summed up at most every two seconds
    }
    ui.last_log_ticks = now;
    const std::uint64_t misses = session.disk_misses();
    if (misses != ui.logged_disk_misses) {
        toh::app::logging::warning("Audio from disk came late: " +
                                   std::to_string(misses - ui.logged_disk_misses) +
                                   " frames played as silence");
        ui.logged_disk_misses = misses;
    }
    const std::uint64_t dropped = session.engine().record_dropped_frames();
    if (dropped != ui.logged_dropped) {
        toh::app::logging::warning("Recording lost " + std::to_string(dropped - ui.logged_dropped) +
                                   " input frames (the disk or the program was too slow)");
        ui.logged_dropped = dropped;
    }
}

bool is_midi_file(const std::string& path) {
    std::string extension = std::filesystem::u8path(path).extension().u8string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension == ".mid" || extension == ".midi";
}

void import_midi_file(Session& session, UiState& ui, const std::string& path) {
    std::string error;
    if (session.import_midi(std::filesystem::u8path(path), &error)) {
        ui.notice = std::string(T_("Imported: ")) + path;
        fit_view(session, ui.view);
    } else {
        ui.error = std::string(T_("MIDI: ")) + error;
    }
}

// Cut, Copy, Paste and Keep only the selection, for the Edit menu and the region menu.
void draw_clipboard_items(Session& session, UiState& ui) {
    if (ImGui::MenuItem(T_("Cut"), key_label(ui, Action::Cut), false, can_take(session, ui))) {
        cut_selection(session, ui);
    }
    if (ImGui::MenuItem(T_("Copy"), key_label(ui, Action::Copy), false, can_take(session, ui))) {
        copy_selection(session, ui);
    }
    if (ImGui::MenuItem(T_("Paste at the playhead"), key_label(ui, Action::Paste), false,
                        !ui.clipboard.empty())) {
        paste_clipboard(session, ui);
    }
    if (ImGui::MenuItem(T_("Keep only the selection"), key_label(ui, Action::KeepSelection), false,
                        has_range(ui))) {
        keep_selection(session, ui);
    }
    if (ImGui::MenuItem(T_("Delete the selection"), key_label(ui, Action::RemoveRegion), false,
                        has_range(ui))) {
        delete_selection(session, ui);
    }
}

// ---- Playing notes -----------------------------------------------------------------------

// The track that keyboard notes play: the armed instrument track, else a selected one, else the
// track of the selected region, else the first instrument track.
std::size_t live_target(const Session& session, const UiState& ui) {
    const auto& tracks = session.tracks();
    if (ui.armed && session.is_instrument_track(ui.armed_track)) {
        return ui.armed_track;
    }
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        if (tracks[t].selected && session.is_instrument_track(t)) {
            return t;
        }
    }
    if (ui.selected && session.is_instrument_track(ui.selected_track)) {
        return ui.selected_track;
    }
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        if (session.is_instrument_track(t)) {
            return t;
        }
    }
    return Session::kNewTrack;
}

// Notes from MIDI keyboards and, when switched on, from the letter keys:
//   A W S E D F T G Y H U J K O L P  =  C C# D D# E F F# G G# A A# B C C# D D#,  Z / X: octave.
void play_live_notes(Session& session, UiState& ui) {
    session.set_live_track(live_target(session, ui));
    ui.midi.take(ui.midi_events);
    for (const toh::app::MidiEvent& event : ui.midi_events) {
        if (event.on) {
            session.note_on(event.pitch, event.velocity);
        } else {
            session.note_off(event.pitch);
        }
    }
    static const ImGuiKey keys[16] = {ImGuiKey_A, ImGuiKey_W, ImGuiKey_S, ImGuiKey_E,
                                      ImGuiKey_D, ImGuiKey_F, ImGuiKey_T, ImGuiKey_G,
                                      ImGuiKey_Y, ImGuiKey_H, ImGuiKey_U, ImGuiKey_J,
                                      ImGuiKey_K, ImGuiKey_O, ImGuiKey_L, ImGuiKey_P};
    const bool typing = ImGui::GetIO().WantTextInput || ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyAlt;
    for (std::size_t k = 0; k < 16; ++k) {
        const bool down = ui.musical_keyboard && !typing && ImGui::IsKeyDown(keys[k]);
        if (down && ui.key_pitch[k] < 0) {
            ui.key_pitch[k] = std::clamp(12 * (ui.keyboard_octave + 1) + static_cast<int>(k), 0, 127);
            session.note_on(ui.key_pitch[k], 0.8f);
        } else if (!down && ui.key_pitch[k] >= 0) {
            session.note_off(ui.key_pitch[k]);
            ui.key_pitch[k] = -1;
        }
    }
    if (ui.musical_keyboard && !typing) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            ui.keyboard_octave = std::max(ui.keyboard_octave - 1, 0);
        }
        if (ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            ui.keyboard_octave = std::min(ui.keyboard_octave + 1, 8);
        }
    }
}

// Picture files are given to tracks when they are dropped on the window; other files are audio.
bool is_picture_file(const std::string& path) {
    std::string extension = std::filesystem::u8path(path).extension().u8string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return extension == ".png" || extension == ".bmp" || extension == ".jpg" ||
           extension == ".jpeg" || extension == ".gif" || extension == ".webp";
}

std::size_t lane_under(const UiState& ui, float y, std::size_t count);  // with the lanes below

// Gives a dropped picture to the track it was dropped on.
void apply_dropped_picture(Session& session, UiState& ui) {
    if (ui.dropped_picture.empty()) {
        return;
    }
    std::string extension = std::filesystem::u8path(ui.dropped_picture).extension().u8string();
    for (char& c : extension) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    const std::size_t track = lane_under(ui, ui.dropped_y, session.tracks().size());
    if (extension != ".png" && extension != ".bmp") {
        ui.error = T_("Track pictures can be PNG or BMP files.");
    } else if (track < session.tracks().size()) {
        session.set_track_icon(track, "file:" + ui.dropped_picture);
    } else {
        ui.error = T_("Drop the picture onto a track to give it that picture.");
    }
    ui.dropped_picture.clear();
}

// ---- Autosave ----------------------------------------------------------------------------

// Every few minutes, unsaved changes go to the recovery file. Once nothing is unsaved any more
// (saved, or a new project), the recovery file goes.
void autosave_tick(Session& session, UiState& ui) {
    if (ui.recovery_file.empty() || ui.ask_recovery) {
        return;
    }
    const std::uint64_t now = SDL_GetTicks();
    if (now - ui.last_recovery_check < 1000) {
        return;
    }
    ui.last_recovery_check = now;
    if (!session.modified()) {
        if (ui.recovery_written) {
            std::error_code code;
            std::filesystem::remove(ui.recovery_file, code);
            ui.recovery_written = false;
        }
        return;
    }
    const std::uint64_t interval = static_cast<std::uint64_t>(ui.settings.autosave_minutes) * 60000u;
    if (!ui.settings.autosave || session.is_recording() || now - ui.last_autosave_ticks < interval) {
        return;
    }
    ui.last_autosave_ticks = now;
    if (!session.needs_recovery_save()) {
        return;
    }
    std::string error;
    if (session.save_recovery(ui.recovery_file, &error)) {
        ui.recovery_written = true;
        toh::app::logging::info("Autosaved the unsaved changes");
    } else {
        toh::app::logging::warning("Autosave failed: " + error);
    }
}

// After a crash: offers the project that autosave kept.
void draw_recovery_popup(Session& session, UiState& ui) {
    if (ui.ask_recovery && !ImGui::IsPopupOpen("###recovery")) {
        ImGui::OpenPopup("###recovery");
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    const std::string title = std::string(T_("Restore the project?")) + "###recovery";
    if (ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(T_("Last time the program did not close properly.\n"
                                  "Autosave kept the changes that were not saved."));
        ImGui::Spacing();
        if (ImGui::Button(T_("Restore"))) {
            std::string error;
            if (session.restore_recovery(ui.recovery_file, &error)) {
                ui.recovery_written = true;
                ui.notice = T_("The project was restored. Save it to keep it.");
                fit_view(session, ui.view);
            } else {
                ui.error = std::string(T_("Restore: ")) + error;
            }
            ui.ask_recovery = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T_("Discard"))) {
            std::error_code code;
            std::filesystem::remove(ui.recovery_file, code);
            toh::app::logging::info("The recovery file was discarded");
            ui.ask_recovery = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// The window title names the project and shows a star while it has unsaved changes.
void update_window_title(const Session& session, UiState& ui) {
    std::string title = "Tone of Hell";
    const bool modified = session.modified();
    if (!session.project_file().empty() || modified) {
        const std::string name = session.project_file().empty()
                                     ? std::string(T_("Untitled"))
                                     : session.project_file().stem().u8string();
        title = name + (modified ? "*" : "") + " - " + title;
    }
    if (title != ui.window_title) {
        ui.window_title = title;
        SDL_SetWindowTitle(ui.window, title.c_str());
    }
}

void open_piano_roll(UiState& ui, std::size_t track, std::size_t region);  // with the piano roll

void perform_action(Session& session, UiState& ui, PendingFiles& pending, SDL_Window* window,
                    Action action) {
    const float centre_x = ui.view.lane_origin_x + ui.view.lane_width * 0.5f;
    switch (action) {
        case Action::PlayPause:
            session.toggle_play();
            break;
        case Action::Stop:
            session.stop_recording();
            session.stop();
            break;
        case Action::BackToStart:
            session.seek_frames(0);
            ui.view.start_seconds = 0.0;
            break;
        case Action::Record:
            toggle_recording(session, ui);
            break;
        case Action::Reverse:
            session.toggle_reverse();
            break;
        case Action::Loop:
            session.set_loop(!session.loop());
            break;
        case Action::Metronome: {
            toh::app::MetronomeSettings settings = session.metronome();
            settings.enabled = !settings.enabled;
            session.set_metronome(settings);
            break;
        }
        case Action::Split:
            split_at_playhead(session, ui);
            break;
        case Action::RemoveRegion:
            if (has_range(ui)) {
                delete_selection(session, ui);
            } else {
                remove_selected_region(session, ui);
            }
            break;
        case Action::Cut:
            cut_selection(session, ui);
            break;
        case Action::Copy:
            copy_selection(session, ui);
            break;
        case Action::Paste:
            paste_clipboard(session, ui);
            break;
        case Action::KeepSelection:
            keep_selection(session, ui);
            break;
        case Action::JoinRegions:
            join_with_next(session, ui);
            break;
        case Action::Deselect:
            ui.selected = false;
            ui.has_range = false;
            session.clear_track_selection();
            break;
        case Action::AddFiles:
            open_file_dialog(pending, window);
            break;
        case Action::ZoomIn:
            zoom_view(ui.view, 1.5, centre_x);
            break;
        case Action::ZoomOut:
            zoom_view(ui.view, 1.0 / 1.5, centre_x);
            break;
        case Action::ZoomFit:
            fit_view(session, ui.view);
            break;
        case Action::NewTrack:
            add_new_track(session, ui, false);
            break;
        case Action::NewProject:
            unless_unsaved(session, ui, UiState::Then::NewProject);
            break;
        case Action::OpenProject:
            unless_unsaved(session, ui, UiState::Then::OpenProject);
            break;
        case Action::CutTool:
            ui.cut_tool = !ui.cut_tool;
            break;
        case Action::Faster:
            session.faster();
            break;
        case Action::Slower:
            session.slower();
            break;
        case Action::NormalSpeed:
            session.set_speed(1.0);
            break;
        case Action::MusicalKeyboard:
            ui.musical_keyboard = !ui.musical_keyboard;
            break;
        case Action::PianoRoll:
            if (const RegionInfo* region = selected_region(session, ui)) {
                if (region->has_notes()) {
                    open_piano_roll(ui, ui.selected_track, ui.selected_region);
                }
            }
            break;
        case Action::Undo:
            if (ui.drag == DragKind::None && session.undo()) {
                ui.selected = false;
            }
            break;
        case Action::Redo:
            if (ui.drag == DragKind::None && session.redo()) {
                ui.selected = false;
            }
            break;
        case Action::SaveProject:
            save_project(session, ui);
            break;
        case Action::SaveProjectAs:
            open_path_dialog(session, ui, PendingPath::Kind::SaveProject);
            break;
        case Action::FullScreen:
            SDL_SetWindowFullscreen(window, (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) == 0);
            break;
        case Action::Count:
            break;
    }
}

// Runs the actions whose shortcuts were pressed. Does nothing while a text field has the
// keyboard or while a new shortcut is being chosen.
void handle_shortcuts(Session& session, UiState& ui, PendingFiles& pending, SDL_Window* window) {
    if (ImGui::GetIO().WantTextInput || ui.capturing >= 0) {
        return;
    }
    for (std::size_t i = 0; i < toh::app::kActionCount; ++i) {
        const auto action = static_cast<Action>(i);
        // While the letters play notes, shortcuts made of a single letter wait.
        if (ui.musical_keyboard) {
            const Chord chord = parse_chord(ui.settings.shortcut(action));
            if (!chord.ctrl && !chord.alt && chord.key >= ImGuiKey_A && chord.key <= ImGuiKey_Z) {
                continue;
            }
        }
        // While the piano roll has the keyboard, its keys edit notes, not regions.
        if (ui.roll_focused && (action == Action::RemoveRegion || action == Action::Split ||
                                action == Action::JoinRegions || action == Action::Deselect ||
                                action == Action::Cut || action == Action::Copy ||
                                action == Action::Paste || action == Action::KeepSelection)) {
            continue;
        }
        if (chord_pressed(parse_chord(ui.settings.shortcut(action)))) {
            perform_action(session, ui, pending, window, action);
        }
    }
}

// ---- Settings ----------------------------------------------------------------------------

void save_settings(const Session& session, UiState& ui) {
    ui.settings.master_gain = session.master_gain();
    ui.settings.input_gain = session.input_gain();
    ui.settings.record_offset_frames = session.record_offset_frames();
    ui.settings.output_device = session.output_device();
    ui.settings.input_device = session.input_device();
    if (!ui.settings_path.empty()) {
        ui.settings.save(ui.settings_path);
    }
    ui.settings_changed = false;
}

// A text with a fixed identity for Dear ImGui, so that translating it does not reset widgets.
std::string with_id(const char* text, const char* id) { return std::string(text) + "###" + id; }

// A list to choose a device from. Returns true when the choice changed.
bool device_combo(const char* label, const char* id, const std::vector<std::string>& names,
                  std::string& chosen) {
    bool changed = false;
    const char* preview = chosen.empty() ? T_("System default") : chosen.c_str();
    if (ImGui::BeginCombo(with_id(label, id).c_str(), preview)) {
        if (ImGui::Selectable(T_("System default"), chosen.empty()) && !chosen.empty()) {
            chosen.clear();
            changed = true;
        }
        for (const std::string& name : names) {
            if (ImGui::Selectable(name.c_str(), name == chosen) && name != chosen) {
                chosen = name;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void draw_settings_window(Session& session, UiState& ui) {
    if (!ui.show_settings) {
        ui.capturing = -1;
        return;
    }
    const float font = ImGui::GetFontSize();
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(40.0f * font, 0.0f), ImGuiCond_Appearing);
    if (ImGui::Begin(with_id(T_("Settings"), "settings").c_str(), &ui.show_settings,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings) &&
        ImGui::BeginTabBar("settings tabs")) {
        // ---- General ----
        if (ImGui::BeginTabItem(with_id(T_("General"), "general").c_str())) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T_("Language"));
            ImGui::SameLine();
            int language = ui.settings.language == toh::app::Language::Russian ? 1 : 0;
            bool language_changed = ImGui::RadioButton("English", &language, 0);
            ImGui::SameLine();
            language_changed = ImGui::RadioButton("\xD0\xA0\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9", &language, 1) || language_changed;
            if (language_changed) {
                ui.settings.language =
                    language == 1 ? toh::app::Language::Russian : toh::app::Language::English;
                toh::app::set_language(ui.settings.language);
                ui.settings_changed = true;
            }

            ImGui::SeparatorText(T_("Which tracks play"));
            bool selected_only = session.play_selected_only();
            if (ImGui::Checkbox(T_("Play only the selected tracks"), &selected_only)) {
                set_play_selected_only(session, ui, selected_only);
            }
            ImGui::TextDisabled("%s", T_("Click a track name to select it, Ctrl+click to select several. "
                                         "With nothing selected, every track plays."));

            ImGui::SeparatorText(T_("Autosave"));
            if (ImGui::Checkbox(T_("Keep unsaved changes in a recovery file"), &ui.settings.autosave)) {
                ui.settings_changed = true;
            }
            tooltip(T_("If the program closes unexpectedly, the next start offers to restore them.\n"
                       "Your project file changes only when you save."));
            ImGui::BeginDisabled(!ui.settings.autosave);
            ImGui::SetNextItemWidth(12.0f * ImGui::GetFontSize());
            if (ImGui::SliderInt("##autosave minutes", &ui.settings.autosave_minutes, 1, 30,
                                 T_("every %d min"), ImGuiSliderFlags_AlwaysClamp)) {
                ui.settings_changed = true;
            }
            ImGui::EndDisabled();

            ImGui::SeparatorText(T_("Logs"));
            if (ImGui::Checkbox(T_("Write a log file for every session"), &ui.settings.write_logs)) {
                ui.settings_changed = true;
                if (ui.settings.write_logs) {
                    toh::app::logging::start(ui.logs_folder);
                    toh::app::logging::info("Logging switched on");
                } else {
                    toh::app::logging::info("Logging switched off");
                    toh::app::logging::stop();
                }
            }
            tooltip(T_("A log lists what the program did and every error. Send it along when you "
                       "report a problem. The 20 newest logs are kept."));
            ImGui::TextDisabled("%s", ui.logs_folder.u8string().c_str());
            if (ImGui::Button(T_("Open the logs folder"))) {
                open_folder(ui.logs_folder);
            }

            ImGui::SeparatorText(T_("Projects"));
            ImGui::TextUnformatted(T_("Folder for projects (the save and open dialogs start here):"));
            const std::string folder = projects_folder(ui).u8string();
            ImGui::TextDisabled("%s", folder.c_str());
            if (ImGui::Button(T_("Choose a folder..."))) {
                open_path_dialog(session, ui, PendingPath::Kind::ProjectsFolder);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(ui.settings.projects_folder.empty());
            if (ImGui::Button(T_("Back to Documents"))) {
                ui.settings.projects_folder.clear();
                ui.settings_changed = true;
            }
            ImGui::EndDisabled();

            ImGui::SeparatorText(T_("Window"));
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T_("When the program starts"));
            ImGui::SameLine();
            static const char* const window_modes[] = {"normal window", "maximized window",
                                                       "full screen"};
            ImGui::SetNextItemWidth(12.0f * ImGui::GetFontSize());
            if (ImGui::BeginCombo("##start window", T_(window_modes[ui.settings.start_window]))) {
                for (int mode = 0; mode < 3; ++mode) {
                    if (ImGui::Selectable(T_(window_modes[mode]), mode == ui.settings.start_window)) {
                        ui.settings.start_window = mode;
                        ui.settings_changed = true;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextDisabled("%s", T_("F11 switches full screen on and off at any time."));

            ImGui::SeparatorText(T_("Instrument sounds"));
            if (session.soundfont()) {
                ImGui::TextUnformatted(session.soundfont()->path().u8string().c_str());
                ImGui::TextDisabled(T_("%d instruments"), static_cast<int>(session.soundfont()->presets().size()));
            } else {
                ImGui::TextDisabled("%s", T_("Not loaded: instrument tracks use the simple synthesizers."));
            }
            if (ImGui::Button(T_("Choose a SoundFont (.sf2)..."))) {
                open_path_dialog(session, ui, PendingPath::Kind::SoundFont);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(ui.settings.soundfont_path.empty());
            if (ImGui::Button(T_("Back to the built-in sounds"))) {
                ui.settings.soundfont_path.clear();
                ui.settings_changed = true;
                load_soundfont(session, ui, default_soundfont_path());
            }
            ImGui::EndDisabled();
            if (ui.midi.devices().empty()) {
                ImGui::TextDisabled("%s", T_("No MIDI keyboard found."));
            } else {
                for (const std::string& device : ui.midi.devices()) {
                    ImGui::Text(T_("MIDI keyboard: %s"), device.c_str());
                }
            }
            if (ImGui::Button(T_("Look for MIDI keyboards again"))) {
                ui.midi.open_all();
            }

            ImGui::SeparatorText(T_("Metronome"));
            if (ImGui::Checkbox(T_("With the metronome on, keep playing past the end"),
                                &ui.settings.metronome_endless)) {
                session.set_metronome_endless(ui.settings.metronome_endless);
                ui.settings_changed = true;
            }
            tooltip(T_("Handy for practising: playback goes on until you stop it. A loop still loops."));

            ImGui::SeparatorText(T_("Reverse"));
            if (ImGui::Checkbox(T_("The Reverse button starts playback"),
                                &ui.settings.reverse_starts_playback)) {
                session.set_reverse_starts_playback(ui.settings.reverse_starts_playback);
                ui.settings_changed = true;
            }
            tooltip(T_("Off: the button only changes the direction, also while playback is stopped."));
            if (ImGui::Checkbox(T_("Switch reverse off at the start of the timeline"),
                                &ui.settings.reverse_off_at_start)) {
                session.set_reverse_off_at_start(ui.settings.reverse_off_at_start);
                ui.settings_changed = true;
            }
            tooltip(T_("When backwards playback reaches the start and stops, the next play goes forwards."));

            ImGui::SeparatorText(T_("Performance"));
            ImGui::BeginDisabled();
            bool gpu = false;
            ImGui::Checkbox(T_("GPU acceleration for effects (planned together with effects)"), &gpu);
            ImGui::EndDisabled();
            ImGui::TextDisabled("%s", T_("The interface itself is already drawn by the graphics card."));
            ImGui::EndTabItem();
        }

        // ---- Audio ----
        if (ImGui::BeginTabItem(with_id(T_("Audio"), "audio").c_str())) {
            if (!ui.devices_listed) {
                ui.devices = toh::audio::list_devices();
                ui.devices_listed = true;
            }
            std::string output = session.output_device();
            std::string input = session.input_device();
            ImGui::SetNextItemWidth(24.0f * font);
            bool devices_changed = device_combo(T_("Output device"), "output", ui.devices.outputs, output);
            ImGui::SetNextItemWidth(24.0f * font);
            devices_changed = device_combo(T_("Input device"), "input", ui.devices.inputs, input) || devices_changed;
            if (devices_changed) {
                std::string error;
                if (!session.set_devices(output, input, &error)) {
                    ui.error = error;
                }
                ui.settings_changed = true;
            }
            if (ImGui::Button(T_("Refresh the device list"))) {
                ui.devices = toh::audio::list_devices();
            }

            ImGui::Spacing();
            float volume = session.master_gain() * 100.0f;
            ImGui::SetNextItemWidth(24.0f * font);
            if (ImGui::SliderFloat("##output volume", &volume, 0.0f, 200.0f, T_("Output volume: %.0f%%"),
                                   ImGuiSliderFlags_AlwaysClamp)) {
                session.set_master_gain(volume / 100.0f);
                ui.settings_changed = true;
            }
            if (double_clicked()) {
                session.set_master_gain(1.0f);
                ui.settings_changed = true;
            }
            ImGui::TextDisabled("%s", T_("The same as the Master slider in the toolbar."));

            ImGui::Spacing();
            float sensitivity = session.input_gain() * 100.0f;
            ImGui::SetNextItemWidth(24.0f * font);
            if (ImGui::SliderFloat("##input gain", &sensitivity, 0.0f, 400.0f,
                                   T_("Input sensitivity: %.0f%%"), ImGuiSliderFlags_AlwaysClamp)) {
                session.set_input_gain(sensitivity / 100.0f);
                ui.settings_changed = true;
            }
            if (double_clicked()) {
                session.set_input_gain(1.0f);
                ui.settings_changed = true;
            }
            ImGui::TextDisabled("%s", T_("Applied to the input before monitoring and recording."));

            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T_("Input level"));
            ImGui::SameLine();
            draw_meter(session.input_peak(), 16.0f * font, ImGui::GetFrameHeight());
            bool input_open = session.input_enabled();
            if (ImGui::Checkbox(T_("Input device open"), &input_open)) {
                std::string error;
                if (!session.set_input_enabled(input_open, &error) && input_open) {
                    ui.error = error;
                }
            }
            if (!session.input_enabled()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", T_("Open the input to see its level"));
            }
            ImGui::EndTabItem();
        }

        // ---- Shortcuts ----
        if (ImGui::BeginTabItem(with_id(T_("Shortcuts"), "shortcuts").c_str())) {
            ImGui::TextDisabled("%s", T_("Click \"Change\", then press the new key, with Ctrl, Shift or Alt if you like."));
            if (ImGui::BeginTable("shortcut table", 4,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                                      ImGuiTableFlags_SizingStretchProp)) {
                ImGui::TableSetupColumn(T_("Action"), ImGuiTableColumnFlags_WidthStretch, 3.0f);
                ImGui::TableSetupColumn(T_("Shortcut"), ImGuiTableColumnFlags_WidthStretch, 1.5f);
                ImGui::TableSetupColumn("##change", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("##clear", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();
                for (std::size_t i = 0; i < toh::app::kActionCount; ++i) {
                    const auto action = static_cast<Action>(i);
                    std::string& shortcut = ui.settings.shortcuts[i];
                    ImGui::PushID(static_cast<int>(i));
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(T_(toh::app::action_info(action).name));
                    ImGui::TableNextColumn();
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(shortcut.empty() ? T_("none") : shortcut.c_str());

                    ImGui::TableNextColumn();
                    if (ui.capturing == static_cast<int>(i)) {
                        Chord chord;
                        if (capture_chord(chord)) {
                            const std::string text = chord_text(chord);
                            for (std::string& other : ui.settings.shortcuts) {
                                if (other == text) {
                                    other.clear();  // one key, one action
                                }
                            }
                            shortcut = text;
                            ui.capturing = -1;
                            ui.settings_changed = true;
                        }
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.45f, 0.15f, 1.0f));
                        if (ImGui::Button(with_id(T_("Press a key..."), "change").c_str())) {
                            ui.capturing = -1;  // clicking again cancels
                        }
                        ImGui::PopStyleColor();
                    } else if (ImGui::Button(with_id(T_("Change"), "change").c_str())) {
                        ui.capturing = static_cast<int>(i);
                    }
                    ImGui::TableNextColumn();
                    if (ImGui::Button(with_id(T_("Clear"), "clear").c_str()) && !shortcut.empty()) {
                        shortcut.clear();
                        ui.settings_changed = true;
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            if (ImGui::Button(T_("Reset all shortcuts"))) {
                ui.settings.reset_shortcuts();
                ui.capturing = -1;
                ui.settings_changed = true;
            }
            ImGui::EndTabItem();
        } else {
            ui.capturing = -1;
        }
        ImGui::EndTabBar();
    }
    ImGui::End();
}

void draw_menu_bar(Session& session, PendingFiles& pending, PendingExport& exports,
                   SDL_Window* window, UiState& ui) {
    if (!ImGui::BeginMenuBar()) {
        return;
    }

    if (ImGui::BeginMenu(T_("File"))) {
        if (ImGui::MenuItem(T_("New project"), key_label(ui, Action::NewProject))) {
            unless_unsaved(session, ui, UiState::Then::NewProject);
        }
        if (ImGui::MenuItem(T_("Open a project..."), key_label(ui, Action::OpenProject))) {
            unless_unsaved(session, ui, UiState::Then::OpenProject);
        }
        if (ImGui::MenuItem(T_("Save the project"), key_label(ui, Action::SaveProject))) {
            save_project(session, ui);
        }
        if (ImGui::MenuItem(T_("Save the project as..."), key_label(ui, Action::SaveProjectAs))) {
            open_path_dialog(session, ui, PendingPath::Kind::SaveProject);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T_("Add audio files..."), key_label(ui, Action::AddFiles))) {
            open_file_dialog(pending, window);
        }
        if (ImGui::MenuItem(T_("Import a MIDI file..."))) {
            open_path_dialog(session, ui, PendingPath::Kind::ImportMidi);
        }
        if (ImGui::BeginMenu(T_("Export audio"), session.length_frames() > 0)) {
            if (ImGui::MenuItem(T_("WAV, 16-bit..."))) {
                open_export_dialog(exports, window, toh::audio::SampleFormat::Int16);
            }
            if (ImGui::MenuItem(T_("WAV, 24-bit (recommended)..."))) {
                open_export_dialog(exports, window, toh::audio::SampleFormat::Int24);
            }
            if (ImGui::MenuItem(T_("WAV, 32-bit float..."))) {
                open_export_dialog(exports, window, toh::audio::SampleFormat::Float32);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(T_("MP3, 320 kbps..."))) {
                open_export_dialog(exports, window, toh::audio::SampleFormat::Int24, 320);
            }
            if (ImGui::MenuItem(T_("MP3, 192 kbps..."))) {
                open_export_dialog(exports, window, toh::audio::SampleFormat::Int24, 192);
            }
            if (ImGui::MenuItem(T_("MP3, variable bit rate (best quality)..."))) {
                open_export_dialog(exports, window, toh::audio::SampleFormat::Int24, 0);
            }
            ImGui::Separator();
            if (ImGui::MenuItem(T_("MIDI file (the notes)..."))) {
                open_path_dialog(session, ui, PendingPath::Kind::ExportMidi);
            }
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T_("Exit"), "Alt+F4")) {
            unless_unsaved(session, ui, UiState::Then::Quit);
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T_("Edit"))) {
        const RegionInfo* region = selected_region(session, ui);
        const std::string undo_text = session.can_undo()
                                          ? std::string(T_("Undo")) + ": " + T_(session.undo_label().c_str())
                                          : std::string(T_("Undo"));
        if (ImGui::MenuItem(undo_text.c_str(), key_label(ui, Action::Undo), false, session.can_undo())) {
            session.undo();
            ui.selected = false;
        }
        const std::string redo_text = session.can_redo()
                                          ? std::string(T_("Redo")) + ": " + T_(session.redo_label().c_str())
                                          : std::string(T_("Redo"));
        if (ImGui::MenuItem(redo_text.c_str(), key_label(ui, Action::Redo), false, session.can_redo())) {
            session.redo();
            ui.selected = false;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T_("Split at the playhead"), key_label(ui, Action::Split), false, !session.tracks().empty())) {
            split_at_playhead(session, ui);
        }
        if (ImGui::MenuItem(T_("Join with the next region"), key_label(ui, Action::JoinRegions), false,
                            can_join_next(session, ui))) {
            join_with_next(session, ui);
        }
        if (ImGui::MenuItem(T_("Join all regions of the track"), nullptr, false,
                            selected_track_has_regions_to_join(session, ui))) {
            join_selected_track(session, ui);
        }
        if (ImGui::MenuItem(T_("Remove fades"), nullptr, false,
                            region != nullptr && (region->fade_in > 0 || region->fade_out > 0))) {
            session.set_region_fades(ui.selected_track, ui.selected_region, 0, 0);
        }
        if (ImGui::MenuItem(T_("Remove the selected region"), key_label(ui, Action::RemoveRegion), false,
                            region != nullptr && !has_range(ui))) {
            remove_selected_region(session, ui);
        }
        ImGui::Separator();
        draw_clipboard_items(session, ui);
        ImGui::Separator();
        if (ImGui::MenuItem(T_("Deselect"), key_label(ui, Action::Deselect), false,
                            ui.selected || has_range(ui))) {
            ui.selected = false;
            ui.has_range = false;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T_("New track"), key_label(ui, Action::NewTrack))) {
            add_new_track(session, ui, false);
        }
        if (ImGui::MenuItem(T_("New track for recording"))) {
            add_new_track(session, ui, true);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T_("Settings..."))) {
            ui.show_settings = true;
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T_("View"))) {
        const float centre_x = ui.view.lane_origin_x + ui.view.lane_width * 0.5f;
        if (ImGui::MenuItem(T_("Zoom in"), key_label(ui, Action::ZoomIn))) {
            zoom_view(ui.view, 1.5, centre_x);
        }
        if (ImGui::MenuItem(T_("Zoom out"), key_label(ui, Action::ZoomOut))) {
            zoom_view(ui.view, 1.0 / 1.5, centre_x);
        }
        if (ImGui::MenuItem(T_("Show the whole project"), key_label(ui, Action::ZoomFit))) {
            fit_view(session, ui.view);
        }
        ImGui::Separator();
        ImGui::MenuItem(T_("Follow the playhead"), nullptr, &ui.follow);
        const bool full_screen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
        if (ImGui::MenuItem(T_("Full screen"), key_label(ui, Action::FullScreen), full_screen)) {
            SDL_SetWindowFullscreen(window, !full_screen);
        }
        if (ImGui::MenuItem(T_("Magnet: stick to region edges"), nullptr, &ui.settings.magnet)) {
            ui.settings_changed = true;
        }
        if (ImGui::BeginMenu(T_("Grid step"))) {
            static const char* const names[] = {"Bars", "Beats", "1/2 beats", "1/4 beats",
                                                "None (hide the grid)"};
            for (int i = 0; i < 5; ++i) {
                if (ImGui::MenuItem(T_(names[i]), nullptr, static_cast<int>(session.grid_step()) == i)) {
                    session.set_grid_step(static_cast<GridStep>(i));
                }
            }
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T_("Transport"))) {
        bool selected_only = session.play_selected_only();
        if (ImGui::MenuItem(T_("Play only the selected tracks"), nullptr, &selected_only)) {
            set_play_selected_only(session, ui, selected_only);
        }
        if (ImGui::BeginMenu(T_("Speed"))) {
            draw_speed_items(session, ui);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T_(session.is_playing() ? "Pause" : "Play"),
                            key_label(ui, Action::PlayPause))) {
            session.toggle_play();
        }
        if (ImGui::MenuItem(T_("Stop"), key_label(ui, Action::Stop))) {
            session.stop_recording();
            session.stop();
        }
        if (ImGui::MenuItem(T_(session.is_recording() ? "Stop recording" : "Record"),
                            key_label(ui, Action::Record))) {
            toggle_recording(session, ui);
        }
        if (ImGui::MenuItem(T_("Back to the start"), key_label(ui, Action::BackToStart))) {
            session.seek_frames(0);
            ui.view.start_seconds = 0.0;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T_("Reverse"), key_label(ui, Action::Reverse), session.reverse())) {
            session.toggle_reverse();
        }
        if (ImGui::MenuItem(T_("Loop"), key_label(ui, Action::Loop), session.loop())) {
            session.set_loop(!session.loop());
        }
        if (ImGui::MenuItem(T_("Metronome"), key_label(ui, Action::Metronome), session.metronome().enabled)) {
            toh::app::MetronomeSettings settings = session.metronome();
            settings.enabled = !settings.enabled;
            session.set_metronome(settings);
        }
        if (ImGui::MenuItem(T_("Monitor the input"), nullptr, session.monitoring())) {
            session.set_monitoring(!session.monitoring());
        }
        ImGui::EndMenu();
    }

    if (ImGui::BeginMenu(T_("Help"))) {
        if (ImGui::MenuItem(T_("Keyboard and mouse..."))) {
            ui.show_help = true;
        }
        if (ImGui::MenuItem(T_("About Tone of Hell..."))) {
            ui.show_about = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem(T_("Open the logs folder"))) {
            open_folder(ui.logs_folder);
        }
        ImGui::EndMenu();
    }

    ImGui::EndMenuBar();
}

// The windows opened from the Help menu.
void draw_dialogs(UiState& ui) {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                   ImGuiWindowFlags_NoSavedSettings;
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();

    if (ui.show_help) {
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin((std::string(T_("Keyboard and mouse")) + "###help").c_str(), &ui.show_help,
                         flags)) {
            static const char* const rows[][2] = {
                {"Mouse wheel", "Zoom the timeline (when no region is selected)"},
                {"Ctrl + wheel", "Zoom the timeline (always)"},
                {"Shift + wheel", "Scroll the timeline sideways"},
                {"Wheel, region selected", "Scroll the track list up and down"},
                {"Drag a region", "Move it; drag an edge to trim it"},
                {"Drag a top corner square", "Fade in or fade out"},
                {"Alt while dragging", "Do not stick to edges"},
                {"Right-click a region", "Menu: split, remove fades, remove"},
                {"Click the ruler or an empty lane", "Move the playhead"},
                {"Double-click a slider", "Back to its normal value"},
                {"Right-click empty track space", "Menu: add a track"},
                {"Drag a region close to another", "Its edge sticks to the other edge"},
                {"Drag a region up or down", "Move it to another track"},
                {"Double-click in an instrument region", "Add a note there, or remove the one under the mouse"},
                {"Drag the name strip of a region", "Move the region (also onto another track)"},
                {"Drag inside regions", "Select a part of one or more tracks"},
                {"Ctrl+X, Ctrl+C, Ctrl+V", "Cut, copy, paste at the playhead"},
                {"Ctrl+E", "Keep only the selection"},
                {"Piano keys on: A W S E D F T G Y H U J K", "Play the notes C to C; Z and X change the octave"},
                {"R on an instrument track, then Record", "Write down the notes you play"},
                {"Scissors on, click a region", "Cut it where the mouse is"},
                {"J", "Join the selected region with the next one"},
                {"Click a track name", "Select the track (Ctrl: several)"},
                {"Double-click a track name", "Rename the track"},
                {"Click the picture of a track", "Choose a picture for it"},
                {"Drop a PNG file onto a track", "It becomes the picture of that track"},
            };
            if (ImGui::BeginTable("shortcuts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                // The keyboard shortcuts as they are set right now, then the fixed mouse actions.
                for (std::size_t i = 0; i < toh::app::kActionCount; ++i) {
                    const auto action = static_cast<Action>(i);
                    const std::string& shortcut = ui.settings.shortcut(action);
                    if (shortcut.empty()) {
                        continue;
                    }
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(shortcut.c_str());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(T_(toh::app::action_info(action).name));
                }
                for (const auto& row : rows) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(T_(row[0]));
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(T_(row[1]));
                }
                ImGui::EndTable();
            }
        }
        ImGui::End();
    }

    if (ui.show_about) {
        ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (ImGui::Begin((std::string(T_("About Tone of Hell")) + "###about").c_str(), &ui.show_about,
                         flags)) {
            const toh::app::BuildInfo& build = toh::app::build_info();
            ImGui::TextUnformatted(T_("Tone of Hell"));
            ImGui::TextUnformatted(T_("An open source digital audio workstation."));
            ImGui::Spacing();
            ImGui::Text(T_("Version %s, commit %s, built %s"), build.version, build.commit, build.date);
            ImGui::TextUnformatted(T_("Copyright (c) 2026 AmmaArk. MIT license."));
            ImGui::TextUnformatted(T_("https://github.com/AmmaArkf/Tone-of-Hell"));
            ImGui::Spacing();
            ImGui::TextDisabled("%s", T_("Uses Dear ImGui (MIT), SDL (zlib), miniaudio (public domain or MIT-0) and LAME (LGPL, as a separate library)."));
        }
        ImGui::End();
    }
}

// ---- Toolbar -----------------------------------------------------------------------------

// The metronome button, the tempo field and the settings popup.
void draw_metronome(Session& session, UiState& ui) {
    toh::app::MetronomeSettings settings = session.metronome();
    bool changed = false;
    const float font = ImGui::GetFontSize();

    ImGui::SameLine();
    if (toggle_button(T_("Metronome"), settings.enabled, ImVec4(0.55f, 0.35f, 0.80f, 1.0f), false)) {
        settings.enabled = !settings.enabled;
        changed = true;
    }
    tooltip(T_("Click on every beat while playing. Also plays when no file is loaded."));

    ImGui::SameLine();
    auto bpm = static_cast<float>(settings.bpm);
    ImGui::SetNextItemWidth(6.0f * font);
    if (ImGui::DragFloat("##bpm", &bpm, 0.5f, 20.0f, 400.0f, "%.1f BPM",
                         ImGuiSliderFlags_AlwaysClamp)) {
        settings.bpm = static_cast<double>(bpm);
        changed = true;
    }
    tooltip(T_("Tempo of the metronome and the grid. Drag, or double-click to type a value."));

    ImGui::SameLine();
    if (ImGui::Button(T_("...##metronome"))) {
        ImGui::OpenPopup("metronome settings");
    }
    tooltip(T_("Metronome and time signature settings"));

    if (ImGui::BeginPopup("metronome settings")) {
        ImGui::TextUnformatted(T_("Metronome and grid"));
        ImGui::Separator();

        int beats = settings.beats_per_bar;
        ImGui::SetNextItemWidth(7.0f * font);
        if (ImGui::InputInt(T_("Beats per bar"), &beats)) {
            settings.beats_per_bar = std::clamp(beats, 1, 32);
            changed = true;
        }

        static const int units[] = {2, 4, 8, 16};
        ImGui::SetNextItemWidth(7.0f * font);
        if (ImGui::BeginCombo(T_("Beat unit"), std::to_string(settings.beat_unit).c_str())) {
            for (const int unit : units) {
                if (ImGui::Selectable(std::to_string(unit).c_str(), unit == settings.beat_unit)) {
                    settings.beat_unit = unit;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled(T_("Time signature: %d/%d. The first beat of a bar sounds higher."),
                            settings.beats_per_bar, settings.beat_unit);

        ImGui::SetNextItemWidth(7.0f * font);
        float volume = settings.volume * 100.0f;
        if (ImGui::SliderFloat("##click volume", &volume, 0.0f, 200.0f, T_("Click volume: %.0f%%"),
                               ImGuiSliderFlags_AlwaysClamp)) {
            settings.volume = volume / 100.0f;
            changed = true;
        }
        if (double_clicked()) {
            settings.volume = toh::app::MetronomeSettings().volume;
            changed = true;
        }

        if (ImGui::Button(T_("Tap tempo"))) {
            const double now = ImGui::GetTime();
            if (!ui.taps.empty() && now - ui.taps.back() > 2.0) {
                ui.taps.clear();  // a long pause starts a new measurement
            }
            ui.taps.push_back(now);
            if (ui.taps.size() > 8) {
                ui.taps.erase(ui.taps.begin());
            }
            if (ui.taps.size() >= 2) {
                const double interval =
                    (ui.taps.back() - ui.taps.front()) / static_cast<double>(ui.taps.size() - 1);
                if (interval > 0.0) {
                    settings.bpm = std::clamp(60.0 / interval, 20.0, 400.0);
                    changed = true;
                }
            }
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T_("Click in time with the music"));
        ImGui::EndPopup();
    }

    if (changed) {
        session.set_metronome(settings);
    }
}

void draw_toolbar(Session& session, PendingFiles& pending, SDL_Window* window, UiState& ui) {
    const float font = ImGui::GetFontSize();
    const ImVec4 record_red(0.85f, 0.15f, 0.15f, 1.0f);

    // Row 1: transport, metronome, master.
    if (ImGui::Button(T_(session.is_playing() ? "Pause" : "Play"))) {
        session.toggle_play();
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("play menu");
    }
    tooltip(T_("Right-click: playback speed"));
    if (ImGui::BeginPopup("play menu")) {
        ImGui::TextDisabled("%s", T_("Playback"));
        draw_speed_items(session, ui);
        ImGui::EndPopup();
    }
    if (session.speed() != 1.0) {
        // The speed stands out while it is not normal; a click returns to normal.
        ImGui::SameLine();
        char speed_text[32];
        std::snprintf(speed_text, sizeof(speed_text), "x%.2f", session.speed());
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.55f, 0.40f, 0.10f, 1.0f));
        if (ImGui::Button(speed_text)) {
            session.set_speed(1.0);
        }
        ImGui::PopStyleColor();
        tooltip(T_("Playback speed. Click: back to normal. Right-click Play for more."));
    }
    ImGui::SameLine();
    if (ImGui::Button(T_("Stop"))) {
        session.stop_recording();
        session.stop();
    }
    ImGui::SameLine();
    if (toggle_button(T_(session.is_recording() ? "Recording##record" : "Record##record"),
                      session.is_recording(), record_red, false)) {
        toggle_recording(session, ui);
    }
    tooltip(T_("Record the input onto the armed track (the R button of a track) or, when no track\n"
               "is armed, onto a new one. Press again, or Stop, to finish."));
    ImGui::SameLine();
    if (ImGui::Button(T_("...##record"))) {
        ImGui::OpenPopup("record settings");
    }
    tooltip(T_("Recording settings: input, monitoring, latency correction"));
    if (ImGui::BeginPopup("record settings")) {
        draw_record_settings(session, ui);
        ImGui::EndPopup();
    }
    if (session.input_enabled()) {
        ImGui::SameLine();
        draw_meter(session.input_peak(), 5.0f * ImGui::GetFontSize(), ImGui::GetFrameHeight());
    }
    ImGui::SameLine();
    if (toggle_button(T_("Reverse"), session.reverse(), ImVec4(0.80f, 0.45f, 0.20f, 1.0f), false)) {
        session.toggle_reverse();
    }
    tooltip(T_("Play backwards from the playhead. Press again to play forwards."));
    ImGui::SameLine();
    bool loop = session.loop();
    if (ImGui::Checkbox(T_("Loop"), &loop)) {
        session.set_loop(loop);
    }

    ImGui::SameLine();
    ImGui::Text(T_("%s / %s"), format_time(session.position_frames(), session.sample_rate()).c_str(),
                format_time(session.length_frames(), session.sample_rate()).c_str());

    draw_metronome(session, ui);

    // The name and the value are written on the slider itself: "Master: 83%".
    ImGui::SameLine();
    float percent = session.master_gain() * 100.0f;
    ImGui::SetNextItemWidth(11.0f * font);
    if (ImGui::SliderFloat("##master", &percent, 0.0f, 200.0f, T_("Master: %.0f%%"),
                           ImGuiSliderFlags_AlwaysClamp)) {
        session.set_master_gain(percent / 100.0f);
    }
    if (double_clicked()) {
        session.set_master_gain(1.0f);
    }
    tooltip(T_("Master volume. 100% leaves the mix unchanged. Double-click: back to 100%. Ctrl+click: type a value."));
    ImGui::SameLine();
    draw_meter(session.master_peak(), 8.0f * font, ImGui::GetFrameHeight());

    const std::uint64_t clips = session.master_clip_count();
    if (clips > ui.seen_clips) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.15f, 0.15f, 1.0f));
        if (ImGui::SmallButton(T_("CLIP"))) {
            ui.seen_clips = clips;  // click to reset the indicator
        }
        ImGui::PopStyleColor();
        tooltip(T_("The mix went above full scale. Lower the faders. Click to reset."));
    }

    // Row 2: files, grid, zoom.
    if (ImGui::Button(T_("Add files..."))) {
        open_file_dialog(pending, window);
    }
    tooltip(T_("Every file becomes a new track. You can also drop files onto the window."));

    ImGui::SameLine();
    if (magnet_button(ui.settings.magnet)) {
        ui.settings.magnet = !ui.settings.magnet;
        ui.settings_changed = true;
    }
    tooltip(T_(ui.settings.magnet
                   ? "Magnet on: dragged regions stick to the edges of other regions.\n"
                     "Click to switch it off, or hold Alt while dragging."
                   : "Magnet off: regions move freely. Click to switch it on."));
    ImGui::SameLine();
    if (scissors_button(ui.cut_tool)) {
        ui.cut_tool = !ui.cut_tool;
    }
    tooltip(T_(ui.cut_tool ? "Scissors on: click a region to cut it where the mouse is.\n"
                             "Click to switch them off."
                           : "Scissors off. Click to switch them on, then click a region to cut it."));
    ImGui::SameLine();
    if (toggle_button(T_("Piano keys"), ui.musical_keyboard, ImVec4(0.55f, 0.35f, 0.80f, 1.0f), false)) {
        ui.musical_keyboard = !ui.musical_keyboard;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(T_("Play notes with the letter keys (Ctrl+K): A W S E D F T G Y H U J K = "
                             "C to C, Z and X: octave down and up (now C%d).\n"
                             "MIDI keyboards play all the time. Notes go to the armed or selected "
                             "instrument track;\narm it (R) and press Record to write them down."),
                          ui.keyboard_octave);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(T_("Grid:"));
    ImGui::SameLine();
    static const char* const step_names[] = {"bars", "beats", "1/2 beats", "1/4 beats",
                                             "none (no grid)"};
    const int current_step = static_cast<int>(session.grid_step());
    ImGui::SetNextItemWidth(8.5f * font);
    if (ImGui::BeginCombo("##grid", T_(step_names[current_step]))) {
        for (int i = 0; i < 5; ++i) {
            if (ImGui::Selectable(T_(step_names[i]), i == current_step)) {
                session.set_grid_step(static_cast<GridStep>(i));
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    const float centre_x = ui.view.lane_origin_x + ui.view.lane_width * 0.5f;
    if (ImGui::Button(T_("-##zoom"))) {
        zoom_view(ui.view, 1.0 / 1.5, centre_x);
    }
    tooltip(T_("Zoom out (Ctrl + mouse wheel)"));
    ImGui::SameLine();
    if (ImGui::Button(T_("+##zoom"))) {
        zoom_view(ui.view, 1.5, centre_x);
    }
    tooltip(T_("Zoom in (Ctrl + mouse wheel)"));
    ImGui::SameLine();
    if (ImGui::Button(T_("Fit"))) {
        fit_view(session, ui.view);
    }
    tooltip(T_("Show the whole project"));
    ImGui::SameLine();
    ImGui::Checkbox(T_("Follow"), &ui.follow);
    tooltip(T_("Keep the playhead in view while playing"));

    ImGui::SameLine();
    ImGui::TextDisabled("%s", T_("(?)"));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "%s",
            T_("Drag a region to move it and its left or right edge to trim it.\n"
               "Drag the small squares in its top corners to make fades.\n"
               "Right-click a region for a menu. Ctrl + wheel zooms, Shift + wheel scrolls.\n"
               "All keyboard shortcuts are listed in Help and can be changed in Edit > Settings."));
    }

    if (!session.audio_running()) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.25f, 1.0f), "%s", T_("No audio device: playback is silent."));
    }
    if (!ui.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.35f, 1.0f), "%s", ui.error.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(T_("Dismiss"))) {
            ui.error.clear();
        }
    }
    if (!ui.notice.empty()) {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.50f, 1.0f), "%s", ui.notice.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(T_("OK##notice"))) {
            ui.notice.clear();
        }
    }
}

// ---- Timeline ----------------------------------------------------------------------------

// Vertical lines at bars and beats. With `labels`, bar numbers are written next to the lines.
void draw_grid(const Session& session, const View& view, ImDrawList* draw_list, float top,
               float bottom, bool labels) {
    if (session.grid_step() == GridStep::None) {
        return;  // the grid is switched off
    }
    const double rate = static_cast<double>(session.sample_rate());
    const double bar_seconds = session.bar_frames() / rate;
    const double beat_seconds = session.beat_frames() / rate;
    if (!(bar_seconds > 0.0) || !(beat_seconds > 0.0)) {
        return;
    }
    const float left = view.lane_origin_x;
    const float right = left + view.lane_width;
    const double end_seconds =
        view.start_seconds + static_cast<double>(view.lane_width) / view.pixels_per_second;

    // Beats, when there is room for them.
    if (beat_seconds * view.pixels_per_second >= 10.0) {
        const auto first = static_cast<std::int64_t>(std::floor(view.start_seconds / beat_seconds));
        for (std::int64_t beat = first;; ++beat) {
            const double seconds = static_cast<double>(beat) * beat_seconds;
            if (seconds > end_seconds) {
                break;
            }
            const float x =
                left + static_cast<float>((seconds - view.start_seconds) * view.pixels_per_second);
            if (x >= left && x <= right) {
                draw_list->AddLine(ImVec2(x, top), ImVec2(x, bottom), IM_COL32(255, 255, 255, 16));
            }
        }
    }

    // Bars; when they get too close, only every 2nd, 4th, 8th ... is drawn.
    std::int64_t every = 1;
    while (bar_seconds * static_cast<double>(every) * view.pixels_per_second < 48.0 &&
           every < (std::int64_t{1} << 20)) {
        every *= 2;
    }
    const double step = bar_seconds * static_cast<double>(every);
    const auto first = static_cast<std::int64_t>(std::floor(view.start_seconds / step));
    for (std::int64_t index = first;; ++index) {
        const double seconds = static_cast<double>(index) * step;
        if (seconds > end_seconds) {
            break;
        }
        const float x =
            left + static_cast<float>((seconds - view.start_seconds) * view.pixels_per_second);
        if (x < left || x > right) {
            continue;
        }
        draw_list->AddLine(ImVec2(x, top), ImVec2(x, bottom), IM_COL32(255, 255, 255, 46));
        if (labels) {
            char label[24];
            std::snprintf(label, sizeof(label), "%lld", static_cast<long long>(index * every + 1));
            draw_list->AddText(ImVec2(x + 3.0f, top), IM_COL32(200, 200, 210, 255), label);
        }
    }
}

void draw_playhead(const Session& session, const View& view, ImDrawList* draw_list, float top,
                   float bottom) {
    const float x = frame_to_x(session, view, session.position_frames());
    if (x >= view.lane_origin_x && x <= view.lane_origin_x + view.lane_width) {
        draw_list->AddLine(ImVec2(x, top), ImVec2(x, bottom), IM_COL32(255, 255, 255, 230), 1.5f);
    }
}

// The ruler: bar numbers on top, time below. Clicking or dragging moves the playhead.
void draw_ruler(Session& session, UiState& ui, float header_width) {
    View& view = ui.view;
    const float font = ImGui::GetFontSize();
    // Without a grid there is no row of bar numbers, so the ruler is lower and the tracks
    // move up.
    const bool grid = session.grid_step() != GridStep::None;
    const float height = (grid ? 2.5f : 1.35f) * font;
    ImGui::Dummy(ImVec2(header_width, height));
    ImGui::SameLine();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    view.lane_origin_x = origin.x;
    view.lane_width = width;

    ImGui::InvisibleButton("ruler", ImVec2(width, height));
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemActivated() || (ImGui::IsItemActive() && io.MouseDelta.x != 0.0f)) {
        session.seek_frames(x_to_frame(session, view, io.MousePos.x));
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float bottom = origin.y + height;
    const float time_top = grid ? origin.y + height * 0.5f : origin.y;  // top of the time row
    draw_list->AddRectFilled(origin, ImVec2(origin.x + width, bottom), IM_COL32(40, 42, 48, 255));
    draw_list->PushClipRect(origin, ImVec2(origin.x + width, bottom), true);
    draw_grid(session, view, draw_list, origin.y, origin.y + height * 0.5f, true);

    // Time marks: pick a distance that keeps the labels apart.
    static const double steps[] = {0.001, 0.005, 0.01, 0.05, 0.1, 0.5, 1,  2,
                                   5,     10,    15,   30,   60,  120, 300, 600};
    double step = steps[0];
    for (const double candidate : steps) {
        step = candidate;
        if (candidate * view.pixels_per_second >= 5.5 * static_cast<double>(font)) {
            break;
        }
    }
    const double end_seconds = view.start_seconds + static_cast<double>(width) / view.pixels_per_second;
    const auto first = static_cast<std::int64_t>(std::floor(view.start_seconds / step));
    for (std::int64_t index = first;; ++index) {
        const double t = static_cast<double>(index) * step;
        if (t > end_seconds) {
            break;
        }
        const float x = origin.x + static_cast<float>((t - view.start_seconds) * view.pixels_per_second);
        draw_list->AddLine(ImVec2(x, (time_top + bottom) * 0.5f), ImVec2(x, bottom),
                           IM_COL32(150, 150, 160, 255));
        char label[32];
        const int minutes = static_cast<int>(t / 60.0);
        const double rest = t - minutes * 60.0;
        if (step < 0.01) {
            std::snprintf(label, sizeof(label), "%d:%06.3f", minutes, rest);
        } else if (step < 1.0) {
            std::snprintf(label, sizeof(label), "%d:%05.2f", minutes, rest);
        } else {
            std::snprintf(label, sizeof(label), "%d:%02d", minutes, static_cast<int>(rest + 0.5));
        }
        draw_list->AddText(ImVec2(x + 3.0f, time_top - 1.0f),
                           IM_COL32(170, 170, 180, 255), label);
    }
    draw_playhead(session, view, draw_list, origin.y, bottom);
    draw_list->PopClipRect();
}

// Edges of other regions (on every track), the playhead and the start of the timeline attract
// a dragged edge that comes within `reach` pixels. Returns the nearest such frame, or -1.
// `held` is the edge it is stuck to already, or -1: that one holds on until the mouse is
// clearly away from it, so the region does not flicker between nearby edges.
std::int64_t nearest_magnet(const Session& session, const View& view, std::size_t track_index,
                            std::size_t region_index, std::int64_t frame, float reach,
                            std::int64_t held, float* distance = nullptr) {
    const float x = frame_to_x(session, view, frame);
    if (held >= 0) {
        const float d = std::fabs(frame_to_x(session, view, held) - x);
        if (d <= 2.0f * reach) {
            if (distance != nullptr) {
                *distance = d;
            }
            return held;
        }
    }
    float best_distance = reach;
    std::int64_t best = -1;
    const auto consider = [&](std::int64_t candidate) {
        const float d = std::fabs(frame_to_x(session, view, candidate) - x);
        if (d <= best_distance) {
            best_distance = d;
            best = candidate;
        }
    };
    consider(0);
    if (!session.is_playing()) {
        consider(session.position_frames());  // a moving playhead would make regions twitch
    }
    for (std::size_t t = 0; t < session.tracks().size(); ++t) {
        const auto& regions = session.tracks()[t].regions;
        for (std::size_t r = 0; r < regions.size(); ++r) {
            if (t == track_index && r == region_index) {
                continue;
            }
            consider(regions[r].start);
            consider(regions[r].end());
        }
    }
    if (distance != nullptr) {
        *distance = best_distance;
    }
    return best;
}

// The track whose lane is at height `y` on the screen, or `count` when there is none.
std::size_t lane_under(const UiState& ui, float y, std::size_t count) {
    for (std::size_t t = 0; t < count && t < ui.lane_spans.size(); ++t) {
        if (y >= ui.lane_spans[t].first && y < ui.lane_spans[t].second) {
            return t;
        }
    }
    return count;
}

// The strip along the top of a region that shows its name; dragging it moves the region.
float region_header_height() { return ImGui::GetFontSize() + 4.0f; }

// The frame under x for an edge of a selected part. With the magnet on it sticks to grid
// lines, region edges of `track` and the playhead nearby (Alt: no sticking).
std::int64_t range_frame(const Session& session, const UiState& ui, float x, std::size_t track) {
    const View& view = ui.view;
    const std::int64_t frame = std::max<std::int64_t>(x_to_frame(session, view, x), 0);
    if (!ui.settings.magnet || ImGui::GetIO().KeyAlt) {
        return frame;
    }
    float best = 0.5f * ImGui::GetFontSize();
    std::int64_t result = frame;
    const auto consider = [&](std::int64_t candidate) {
        const float distance = std::fabs(frame_to_x(session, view, candidate) - x);
        if (distance <= best) {
            best = distance;
            result = candidate;
        }
    };
    consider(session.snap_frame(frame));
    consider(session.position_frames());
    if (track < session.tracks().size()) {
        for (const RegionInfo& region : session.tracks()[track].regions) {
            consider(region.start);
            consider(region.end());
        }
    }
    return result;
}

// What is under the mouse inside a lane.
struct Hit {
    DragKind kind = DragKind::None;
    std::size_t region = 0;
};

Hit hit_test(const Session& session, const View& view, const TrackInfo& track, const ImVec2& mouse,
             float lane_top) {
    const float handle = 0.5f * ImGui::GetFontSize();  // reach of the edges and fade handles
    // Later regions are drawn on top, so they are tested first.
    for (std::size_t r = track.regions.size(); r-- > 0;) {
        const RegionInfo& region = track.regions[r];
        const float x0 = frame_to_x(session, view, region.start);
        const float x1 = frame_to_x(session, view, region.end());
        if (mouse.x < x0 || mouse.x > x1) {
            continue;
        }
        Hit hit;
        hit.region = r;
        const bool in_top = mouse.y < lane_top + 1.6f * handle;
        const float fade_in_x = frame_to_x(session, view, region.start + region.fade_in);
        const float fade_out_x = frame_to_x(session, view, region.end() - region.fade_out);
        const bool wide = x1 - x0 > 4.0f * handle;
        if (in_top && std::fabs(mouse.x - fade_in_x) <= handle) {
            hit.kind = DragKind::FadeIn;
        } else if (in_top && std::fabs(mouse.x - fade_out_x) <= handle) {
            hit.kind = DragKind::FadeOut;
        } else if (wide && mouse.x < x0 + handle) {
            hit.kind = DragKind::TrimStart;
        } else if (wide && mouse.x > x1 - handle) {
            hit.kind = DragKind::TrimEnd;
        } else if (mouse.y < lane_top + region_header_height()) {
            hit.kind = DragKind::Move;  // the name strip
        } else {
            hit.kind = DragKind::Select;  // the body: select a part
        }
        return hit;
    }
    return Hit();
}

// Note regions show the pitches 48 to 84 (C3 to C6) from bottom to top.
constexpr int kNoteRangeLow = 48;
constexpr int kNoteRangeHigh = 84;

float note_y(int pitch, float top, float height) {
    const int clamped = std::clamp(pitch, kNoteRangeLow, kNoteRangeHigh);
    const float t = static_cast<float>(clamped - kNoteRangeLow) /
                    static_cast<float>(kNoteRangeHigh - kNoteRangeLow);
    return top + height - 4.0f - t * (height - 8.0f);
}

int y_to_pitch(float y, float top, float height) {
    const float t = (top + height - 4.0f - y) / (height - 8.0f);
    const int pitch = kNoteRangeLow + static_cast<int>(std::lround(
                                          t * static_cast<float>(kNoteRangeHigh - kNoteRangeLow)));
    return std::clamp(pitch, kNoteRangeLow, kNoteRangeHigh);
}

// The name written on a region, with its speed when that is not normal.
std::string region_title(const RegionInfo& region) {
    if (region.rate == 1.0) {
        return region.source->name;
    }
    char speed[32];
    std::snprintf(speed, sizeof(speed), "  x%.2f", region.rate);
    return region.source->name + speed;
}

void draw_region(const Session& session, const View& view, const TrackInfo& track,
                 const RegionInfo& region, bool selected, bool show_handles, float top,
                 float height) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const float left_edge = view.lane_origin_x;
    const float right_edge = left_edge + view.lane_width;
    const float x0 = frame_to_x(session, view, region.start);
    const float x1 = frame_to_x(session, view, region.end());
    if (x1 < left_edge || x0 > right_edge) {
        return;
    }
    const float bottom = top + height;
    const ImVec2 low(x0, top + 1.0f);
    const ImVec2 high(std::max(x1, x0 + 1.0f), bottom - 1.0f);
    draw_list->AddRectFilled(low, high,
                             region.has_notes()
                                 ? (selected ? IM_COL32(92, 70, 120, 255) : IM_COL32(70, 52, 96, 255))
                                 : (selected ? IM_COL32(58, 84, 116, 255) : IM_COL32(44, 62, 84, 255)),
                             3.0f);

    // Waveform: one vertical line per pixel column, only for the visible part.
    const ImU32 wave = track.mute ? IM_COL32(110, 120, 130, 255) : IM_COL32(120, 190, 255, 255);
    const float middle = top + height * 0.5f;
    const float half = height * 0.5f - 3.0f;
    const int first_column = static_cast<int>(std::floor(std::max(x0, left_edge)));
    const int last_column = static_cast<int>(std::ceil(std::min(x1, right_edge)));
    const std::int64_t source_first = region.offset;
    const std::int64_t source_last = region.offset + region.source_frames() - 1;
    const auto to_source = [&region](std::int64_t timeline_frame) {
        return region.offset + static_cast<std::int64_t>(
                                   static_cast<double>(timeline_frame - region.start) * region.rate);
    };
    for (int column = first_column; column < last_column; ++column) {
        std::int64_t first = to_source(x_to_frame(session, view, static_cast<float>(column)));
        std::int64_t last = to_source(x_to_frame(session, view, static_cast<float>(column + 1)) - 1);
        first = std::max(first, source_first);
        last = std::min(std::max(last, first), source_last);
        float low_value = 0.0f;
        float high_value = 0.0f;
        if (first > source_last || !region.source->range(first, last, low_value, high_value)) {
            continue;
        }
        low_value = std::clamp(low_value, -1.0f, 1.0f);
        high_value = std::clamp(high_value, -1.0f, 1.0f);
        const float px = static_cast<float>(column) + 0.5f;
        draw_list->AddLine(ImVec2(px, middle - high_value * half),
                           ImVec2(px, middle - low_value * half + 1.0f), wave);
    }

    // Notes: a bar for every note, its height showing the pitch.
    if (region.has_notes()) {
        const float row = std::max(2.0f, (height - 8.0f) / static_cast<float>(kNoteRangeHigh - kNoteRangeLow + 1));
        for (const toh::core::Note& note : region.source->clip->notes->notes) {
            const std::int64_t from = std::max(note.start, region.offset);
            const std::int64_t to = std::min(note.start + note.length, region.offset + region.length);
            if (from >= to) {
                continue;
            }
            const float nx0 = frame_to_x(session, view, region.start + (from - region.offset));
            const float nx1 =
                std::max(frame_to_x(session, view, region.start + (to - region.offset)), nx0 + 2.0f);
            if (nx1 < left_edge || nx0 > right_edge) {
                continue;
            }
            const float y = note_y(note.pitch, top, height);
            draw_list->AddRectFilled(ImVec2(nx0, y - row * 0.5f), ImVec2(nx1, y + row * 0.5f),
                                     IM_COL32(255, 190, 110, 230), 1.0f);
        }
    }

    // Fades: the part that is turned down is shaded, with a line along the fade.
    const float fade_in_x = frame_to_x(session, view, region.start + region.fade_in);
    const float fade_out_x = frame_to_x(session, view, region.end() - region.fade_out);
    const ImU32 shade = IM_COL32(10, 12, 16, 150);
    const ImU32 fade_line = IM_COL32(235, 235, 245, 200);
    // The fade follows its shape: level 0 at high.y, full level at low.y.
    const auto draw_fade = [&](float from_x, float to_x, bool rising) {
        constexpr int kSteps = 24;
        ImVec2 points[kSteps + 1];
        for (int k = 0; k <= kSteps; ++k) {
            const float t = static_cast<float>(k) / static_cast<float>(kSteps);
            const float level = toh::core::fade_curve(region.fade_shape, rising ? t : 1.0f - t);
            points[k] = ImVec2(from_x + (to_x - from_x) * t, high.y + (low.y - high.y) * level);
        }
        for (int k = 0; k < kSteps; ++k) {
            // The turned-down part, between the curve and the full level.
            draw_list->AddQuadFilled(points[k], points[k + 1], ImVec2(points[k + 1].x, low.y),
                                     ImVec2(points[k].x, low.y), shade);
        }
        draw_list->AddPolyline(points, kSteps + 1, fade_line, ImDrawFlags_None, 1.5f);
    };
    if (region.fade_in > 0) {
        draw_fade(x0, fade_in_x, true);
    }
    if (region.fade_out > 0) {
        draw_fade(fade_out_x, x1, false);
    }

    // The name strip on top: grab it to move the region.
    const float strip_bottom = std::min(top + 1.0f + region_header_height(), bottom - 1.0f);
    draw_list->AddRectFilled(low, ImVec2(high.x, strip_bottom),
                             region.has_notes()
                                 ? (selected ? IM_COL32(130, 100, 170, 235) : IM_COL32(52, 38, 72, 235))
                                 : (selected ? IM_COL32(88, 124, 170, 235) : IM_COL32(30, 44, 62, 235)),
                             3.0f, ImDrawFlags_RoundCornersTop);
    draw_list->AddLine(ImVec2(low.x, strip_bottom), ImVec2(high.x, strip_bottom),
                       IM_COL32(84, 104, 134, 200));

    draw_list->AddRect(low, high, selected ? IM_COL32(210, 228, 255, 255) : IM_COL32(84, 104, 134, 255),
                       3.0f);

    if (show_handles) {
        const float size = 0.3f * ImGui::GetFontSize();
        for (const float x : {fade_in_x, fade_out_x}) {
            draw_list->AddRectFilled(ImVec2(x - size, low.y), ImVec2(x + size, low.y + 2.0f * size),
                                     IM_COL32(235, 235, 245, 255), 1.0f);
        }
    }

    // The name of the source, when there is room for it.
    const float text_x = std::max(x0, left_edge) + 6.0f;
    const float room = high.x - text_x - 6.0f;
    if (room > 3.0f * ImGui::GetFontSize()) {
        draw_list->AddText(ImVec2(text_x, low.y + 2.0f), IM_COL32(225, 232, 245, 210),
                           fit_text(region_title(region), room).c_str());
    }
}

// One lane: draws the regions of a track and handles the mouse on them.
void draw_lane(Session& session, UiState& ui, std::size_t track_index, float row_height) {
    const View& view = ui.view;
    const TrackInfo& track = session.tracks()[track_index];
    const ImGuiIO& io = ImGui::GetIO();

    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = view.lane_width;
    ImGui::InvisibleButton("lane", ImVec2(width, row_height));
    const bool hovered = ImGui::IsItemHovered();
    const bool dragging_here = ui.drag != DragKind::None && ui.drag_owner == track_index;
    if (ui.lane_spans.size() <= track_index) {
        ui.lane_spans.resize(track_index + 1);
    }
    ui.lane_spans[track_index] = {origin.y, origin.y + row_height};

    Hit hit;
    if (hovered && ui.drag == DragKind::None) {
        hit = hit_test(session, view, track, io.MousePos, origin.y);
        if (ui.cut_tool) {
            if (hit.kind != DragKind::None) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
            }
        } else if (hit.kind == DragKind::TrimStart || hit.kind == DragKind::TrimEnd) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else if (hit.kind == DragKind::FadeIn || hit.kind == DragKind::FadeOut) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        } else if (hit.kind == DragKind::Move) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        } else if (hit.kind == DragKind::Select) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
        }
    }

    // Start of a click or drag.
    if (ImGui::IsItemActivated() && ui.cut_tool && hit.kind != DragKind::None) {
        // The scissors: cut the region under the mouse where it is (or at an edge nearby).
        std::int64_t frame = x_to_frame(session, view, io.MousePos.x);
        if (ui.settings.magnet && !io.KeyAlt) {
            const std::int64_t magnet = nearest_magnet(session, view, track_index, hit.region,
                                                       frame, 0.5f * ImGui::GetFontSize(), -1);
            if (magnet >= 0) {
                frame = magnet;
            }
        }
        session.split_region(track_index, hit.region, frame);
        ui.selected = false;
    } else if (ImGui::IsItemActivated()) {
        ui.drag_track = track_index;
        ui.drag_owner = track_index;
        ui.focus_track = track_index;
        ui.has_range = false;
        if (hit.kind == DragKind::None) {
            ui.drag = DragKind::Seek;
            ui.selected = false;
            session.seek_frames(x_to_frame(session, view, io.MousePos.x));
        } else {
            ui.drag = hit.kind;
            ui.drag_region = hit.region;
            ui.selected = true;
            ui.selected_track = track_index;
            ui.selected_region = hit.region;
            ui.drag_grab =
                x_to_frame(session, view, io.MousePos.x) - track.regions[hit.region].start;
            if (hit.kind == DragKind::Select) {
                ui.range_anchor = range_frame(session, ui, io.MousePos.x, track_index);
                ui.range_first = track_index;
                ui.range_last = track_index;
            }
        }
    }

    // Right-click: select the region and open its menu.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hit.kind != DragKind::None) {
        ui.selected = true;
        ui.selected_track = track_index;
        ui.selected_region = hit.region;
        ImGui::OpenPopup("region menu");
    } else if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ui.open_track_menu = true;  // empty part of the lane
    }
    if (ImGui::BeginPopup("region menu")) {
        const RegionInfo* region = selected_region(session, ui);
        const std::int64_t playhead = session.position_frames();
        draw_clipboard_items(session, ui);
        ImGui::Separator();
        const bool can_split =
            region != nullptr && playhead > region->start && playhead < region->end();
        if (ImGui::MenuItem(T_("Split at the playhead"), key_label(ui, Action::Split), false, can_split)) {
            split_at_playhead(session, ui);
        }
        if (ImGui::MenuItem(T_("Join with the next region"), key_label(ui, Action::JoinRegions), false,
                            can_join_next(session, ui))) {
            join_with_next(session, ui);
        }
        if (ImGui::MenuItem(T_("Join all regions of the track"), nullptr, false,
                            selected_track_has_regions_to_join(session, ui))) {
            join_selected_track(session, ui);
        }
        if (ImGui::MenuItem(T_("Remove fades"), nullptr, false,
                            region != nullptr && (region->fade_in > 0 || region->fade_out > 0))) {
            session.set_region_fades(ui.selected_track, ui.selected_region, 0, 0);
        }
        if (region != nullptr && region->has_notes() &&
            ImGui::MenuItem(T_("Open the piano roll"), key_label(ui, Action::PianoRoll))) {
            open_piano_roll(ui, ui.selected_track, ui.selected_region);
        }
        if (region != nullptr && region->has_notes() && ImGui::BeginMenu(T_("Instrument"))) {
            const TrackInfo& owner = session.tracks()[ui.selected_track];
            int program = owner.program;
            toh::core::Instrument instrument = owner.instrument;
            if (instrument_menu(session, owner.program, owner.instrument, program, instrument)) {
                session.set_track_program(ui.selected_track, program);
                session.set_track_instrument(ui.selected_track, instrument);
            }
            ImGui::Separator();
            ImGui::TextDisabled("%s", T_("Double-click in the region: add or remove a note"));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T_("Region speed"), region != nullptr && !region->has_notes())) {
            float percent = static_cast<float>(region->rate * 100.0);
            ImGui::SetNextItemWidth(14.0f * ImGui::GetFontSize());
            if (ImGui::SliderFloat("##region speed", &percent, 25.0f, 400.0f, T_("Speed: %.0f%%"),
                                   ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
                session.set_region_speed(ui.selected_track, ui.selected_region,
                                         std::round(percent) / 100.0);
            }
            if (double_clicked()) {
                session.set_region_speed(ui.selected_track, ui.selected_region, 1.0);
            }
            ImGui::Separator();
            static const int presets[] = {50, 75, 100, 125, 150, 200};
            for (const int preset : presets) {
                char label[32];
                std::snprintf(label, sizeof(label), "%d%%", preset);
                if (ImGui::MenuItem(label, nullptr, std::lround(region->rate * 100.0) == preset)) {
                    session.set_region_speed(ui.selected_track, ui.selected_region, preset / 100.0);
                }
            }
            ImGui::TextDisabled("%s", T_("The region gets shorter or longer and the pitch follows,\n"
                                         "like on a tape. Every region has its own speed."));
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu(T_("Fade shape"), region != nullptr)) {
            static const struct {
                const char* name;
                toh::core::FadeShape shape;
            } shapes[] = {{"Curve (sounds even)", toh::core::FadeShape::Curve},
                          {"Straight line", toh::core::FadeShape::Linear},
                          {"S-curve (soft at both ends)", toh::core::FadeShape::SCurve}};
            for (const auto& choice : shapes) {
                if (ImGui::MenuItem(T_(choice.name), nullptr, region->fade_shape == choice.shape)) {
                    session.set_region_fade_shape(ui.selected_track, ui.selected_region, choice.shape);
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem(T_("Remove the region"), key_label(ui, Action::RemoveRegion), false, region != nullptr)) {
            remove_selected_region(session, ui);
        }
        draw_playback_menu(session, ui);
        ImGui::EndPopup();
    }

    // Double-click in a note region: add a note there, or remove the note under the mouse.
    if (hovered && hit.kind != DragKind::None && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
        hit.region < track.regions.size() && track.regions[hit.region].has_notes()) {
        const RegionInfo& region = track.regions[hit.region];
        const std::int64_t frame = x_to_frame(session, view, io.MousePos.x);
        const std::int64_t at = region.offset + (frame - region.start);
        const int pitch = y_to_pitch(io.MousePos.y, origin.y, row_height);
        std::vector<toh::core::Note> notes = region.source->clip->notes->notes;
        bool removed = false;
        for (auto it = notes.begin(); it != notes.end(); ++it) {
            if (it->pitch == pitch && at >= it->start && at < it->start + it->length) {
                notes.erase(it);
                removed = true;
                break;
            }
        }
        if (!removed) {
            toh::core::Note note;
            note.start = std::max<std::int64_t>(
                region.offset + (session.snap_frame(frame) - region.start), region.offset);
            note.length = std::max<std::int64_t>(std::llround(session.beat_frames()), 1);
            note.pitch = static_cast<std::uint8_t>(pitch);
            note.velocity = 0.8f;
            notes.push_back(note);
        }
        session.set_region_notes(track_index, hit.region, std::move(notes));
        ui.drag = DragKind::None;
    }

    // The drag itself.
    if (dragging_here && ImGui::IsItemActive()) {
        const std::int64_t mouse_frame = x_to_frame(session, view, io.MousePos.x);
        const bool moved = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f);
        // The dragged region may sit on another track than this lane's by now.
        const std::size_t drag_track = ui.drag_track;
        const bool region_exists = drag_track < session.tracks().size() &&
                                   ui.drag_region < session.tracks()[drag_track].regions.size();
        // Regions follow the mouse smoothly; with the magnet on, an edge that comes close to
        // another edge sticks to it. Alt switches the magnet off while it is held.
        const bool magnet_on = ui.settings.magnet && !io.KeyAlt;
        const float reach = 0.5f * ImGui::GetFontSize();
        const auto stuck = [&](std::int64_t frame) {
            const std::int64_t held = ui.magnet_frame;
            ui.magnet_frame = -1;
            if (!magnet_on) {
                return frame;
            }
            const std::int64_t magnet =
                nearest_magnet(session, view, drag_track, ui.drag_region, frame, reach, held);
            ui.magnet_end = false;
            if (magnet >= 0) {
                ui.magnet_frame = magnet;
                return magnet;
            }
            return frame;
        };
        switch (ui.drag) {
            case DragKind::Seek:
                if (io.MouseDelta.x != 0.0f) {
                    session.seek_frames(mouse_frame);
                }
                break;
            case DragKind::Select:
                if (moved) {
                    // From where the drag began to the mouse, over the lanes in between.
                    const std::size_t lane = lane_under(ui, io.MousePos.y, session.tracks().size());
                    if (lane < session.tracks().size()) {
                        ui.range_first = std::min(ui.drag_owner, lane);
                        ui.range_last = std::max(ui.drag_owner, lane);
                    }
                    const std::int64_t frame = range_frame(session, ui, io.MousePos.x, lane);
                    ui.range_begin = std::min(ui.range_anchor, frame);
                    ui.range_end = std::max(ui.range_anchor, frame);
                    ui.has_range = ui.range_end > ui.range_begin;
                    if (ui.has_range) {
                        ui.selected = false;
                    }
                }
                break;
            case DragKind::Move:
                if (moved && region_exists) {
                    // Either end of the region may stick. The end that already sticks keeps
                    // priority; otherwise the closer one wins.
                    const std::int64_t length = session.tracks()[drag_track].regions[ui.drag_region].length;
                    std::int64_t start = std::max<std::int64_t>(mouse_frame - ui.drag_grab, 0);
                    const std::int64_t held = ui.magnet_frame;
                    const bool held_end = ui.magnet_end;
                    ui.magnet_frame = -1;
                    if (magnet_on) {
                        float start_distance = 0.0f;
                        float end_distance = 0.0f;
                        const std::int64_t to_start =
                            nearest_magnet(session, view, drag_track, ui.drag_region, start, reach,
                                           held_end ? -1 : held, &start_distance);
                        const std::int64_t to_end = nearest_magnet(
                            session, view, drag_track, ui.drag_region, start + length, reach,
                            held_end ? held : -1, &end_distance);
                        const bool end_fits = to_end >= 0 && to_end - length >= 0;
                        const bool keep_start = held >= 0 && !held_end && to_start == held;
                        const bool keep_end = held >= 0 && held_end && to_end == held && end_fits;
                        if (keep_start ||
                            (!keep_end && to_start >= 0 && (!end_fits || start_distance <= end_distance))) {
                            start = to_start;
                            ui.magnet_frame = to_start;
                            ui.magnet_end = false;
                        } else if (end_fits) {
                            start = to_end - length;
                            ui.magnet_frame = to_end;
                            ui.magnet_end = true;
                        }
                    }
                    // Over another lane, the region moves onto that track.
                    const std::size_t target =
                        lane_under(ui, io.MousePos.y, session.tracks().size());
                    std::size_t moved_to = ui.drag_region;
                    if (target < session.tracks().size() && target != drag_track &&
                        session.move_region_to_track(drag_track, ui.drag_region, target, start,
                                                     &moved_to)) {
                        ui.drag_track = target;
                        ui.drag_region = moved_to;
                        ui.selected_track = target;
                        ui.selected_region = moved_to;
                    } else {
                        session.move_region(drag_track, ui.drag_region, start);
                    }
                }
                break;
            case DragKind::TrimStart:
                if (moved && region_exists) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    session.trim_region_start(drag_track, ui.drag_region, stuck(mouse_frame));
                }
                break;
            case DragKind::TrimEnd:
                if (moved && region_exists) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    session.trim_region_end(drag_track, ui.drag_region, stuck(mouse_frame));
                }
                break;
            case DragKind::FadeIn:
                if (moved && region_exists) {
                    const RegionInfo& region = track.regions[ui.drag_region];
                    session.set_region_fades(track_index, ui.drag_region,
                                             std::max<std::int64_t>(mouse_frame - region.start, 0),
                                             region.fade_out);
                }
                break;
            case DragKind::FadeOut:
                if (moved && region_exists) {
                    const RegionInfo& region = track.regions[ui.drag_region];
                    // Set the fade-in first so that it is not shortened by a long fade-out.
                    const std::int64_t wanted = std::max<std::int64_t>(region.end() - mouse_frame, 0);
                    const std::int64_t room = region.length - region.fade_in;
                    session.set_region_fades(track_index, ui.drag_region, region.fade_in,
                                             std::min(wanted, room));
                }
                break;
            case DragKind::None:
                break;
        }
    }
    if (dragging_here && ImGui::IsItemDeactivated()) {
        ui.drag = DragKind::None;
        ui.magnet_frame = -1;
    }

    // Drawing.
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 corner(origin.x + width, origin.y + row_height);
    draw_list->AddRectFilled(origin, corner, IM_COL32(28, 30, 34, 255));
    draw_list->PushClipRect(origin, corner, true);
    draw_grid(session, view, draw_list, origin.y, corner.y, false);
    for (std::size_t r = 0; r < track.regions.size(); ++r) {
        const bool selected =
            ui.selected && ui.selected_track == track_index && ui.selected_region == r;
        const bool under_mouse = hovered && hit.kind != DragKind::None && hit.region == r;
        draw_region(session, view, track, track.regions[r], selected, selected || under_mouse,
                    origin.y, row_height);
    }
    // The selected part.
    if (has_range(ui) && track_index >= ui.range_first && track_index <= ui.range_last) {
        const float rx0 = frame_to_x(session, view, ui.range_begin);
        const float rx1 = std::max(frame_to_x(session, view, ui.range_end), rx0 + 1.0f);
        draw_list->AddRectFilled(ImVec2(rx0, origin.y), ImVec2(rx1, corner.y), IM_COL32(235, 240, 255, 55));
        draw_list->AddLine(ImVec2(rx0, origin.y), ImVec2(rx0, corner.y), IM_COL32(235, 240, 255, 210), 1.5f);
        draw_list->AddLine(ImVec2(rx1, origin.y), ImVec2(rx1, corner.y), IM_COL32(235, 240, 255, 210), 1.5f);
    }
    if (track.regions.empty() && !(session.is_recording() && session.record_track() == track_index)) {
        draw_list->AddText(ImVec2(view.lane_origin_x + 8.0f, origin.y + 6.0f),
                           IM_COL32(150, 150, 160, 210),
                           T_("Empty track. Arm it with its R button and press Record."));
    }

    // The take that is being recorded onto this track, growing with the playhead.
    if (session.is_recording() && session.record_track() == track_index) {
        const std::int64_t recorded = session.recorded_frames();
        std::int64_t take_start = session.engine().record_start_frame();
        if (take_start < 0) {
            take_start = session.position_frames() - recorded;
        }
        const float x0 = frame_to_x(session, view, take_start);
        const float x1 = frame_to_x(session, view, take_start + recorded);
        draw_list->AddRectFilled(ImVec2(x0, origin.y + 1.0f),
                                 ImVec2(std::max(x1, x0 + 1.0f), corner.y - 1.0f),
                                 IM_COL32(120, 30, 30, 170), 3.0f);

        // The waveform of what is being recorded. Columns that reach full scale are red.
        const toh::app::Overview& wave = session.take_overview();
        const float middle = (origin.y + corner.y) * 0.5f;
        const float half = (corner.y - origin.y) * 0.5f - 3.0f;
        const int first_column = static_cast<int>(std::floor(std::max(x0, view.lane_origin_x)));
        const int last_column =
            static_cast<int>(std::ceil(std::min(x1, view.lane_origin_x + view.lane_width)));
        for (int column = first_column; column < last_column; ++column) {
            std::int64_t first = x_to_frame(session, view, static_cast<float>(column)) - take_start;
            const std::int64_t last =
                x_to_frame(session, view, static_cast<float>(column + 1)) - 1 - take_start;
            if (last < 0) {
                continue;
            }
            first = std::max<std::int64_t>(first, 0);
            float low = 0.0f;
            float high = 0.0f;
            if (!wave.range(first, std::max(first, last), low, high)) {
                continue;
            }
            const bool clipped = high >= 0.999f || low <= -0.999f;
            low = std::clamp(low, -1.0f, 1.0f);
            high = std::clamp(high, -1.0f, 1.0f);
            const float px = static_cast<float>(column) + 0.5f;
            draw_list->AddLine(ImVec2(px, middle - high * half), ImVec2(px, middle - low * half + 1.0f),
                               clipped ? IM_COL32(255, 70, 60, 255) : IM_COL32(255, 205, 200, 255));
        }
        if (session.take_clipped()) {
            draw_list->AddText(ImVec2(std::max(x0, view.lane_origin_x) + 6.0f, origin.y + 3.0f),
                               IM_COL32(255, 90, 80, 255), T_("Input overloaded"));
        }
    }
    // Where the scissors would cut.
    if (ui.cut_tool && hovered && hit.kind != DragKind::None) {
        const float x = std::floor(io.MousePos.x) + 0.5f;
        draw_list->AddLine(ImVec2(x, origin.y), ImVec2(x, corner.y), IM_COL32(255, 90, 80, 230), 2.0f);
    }
    // Where a dragged edge sticks, across every lane.
    if (ui.drag != DragKind::None && ui.magnet_frame >= 0) {
        const float x = std::floor(frame_to_x(session, view, ui.magnet_frame)) + 0.5f;
        draw_list->AddLine(ImVec2(x, origin.y), ImVec2(x, corner.y), IM_COL32(255, 215, 60, 230),
                           2.0f);
    }
    draw_playhead(session, view, draw_list, origin.y, corner.y);
    draw_list->PopClipRect();
}

// The controls on the left of a lane. Returns true when the track should be removed.
bool draw_track_header(Session& session, UiState& ui, std::size_t i, float header_width,
                       float row_height) {
    const TrackInfo& track = session.tracks()[i];
    const float font = ImGui::GetFontSize();
    bool remove = false;

    // A selected track has a blue header; a track that is not heard is drawn faded.
    const bool dimmed = !session.track_audible(i);
    if (dimmed) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.55f);
    }
    if (track.selected) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.17f, 0.25f, 0.40f, 1.0f));
    }
    ImGui::BeginChild("header", ImVec2(header_width, row_height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);
    if (track.selected) {
        ImGui::PopStyleColor();
    }

    // The picture on the left of the controls: click it to choose another.
    const float icon_size = row_height - 2.0f * ImGui::GetStyle().WindowPadding.y - 2.0f;
    const ImVec2 icon_origin = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("icon", ImVec2(icon_size, icon_size))) {
        ImGui::OpenPopup("track icon");
    }
    const bool icon_hovered = ImGui::IsItemHovered();
    tooltip(T_("Click to choose a picture for this track"));
    draw_track_icon(ui, track.icon, icon_origin, icon_size, icon_hovered);
    draw_icon_popup(session, ui, i);
    ImGui::SameLine();
    ImGui::BeginGroup();
    const float group_x = ImGui::GetCursorPosX();  // SameLine positions are relative to this
    if (toggle_button(T_("M"), track.mute, ImVec4(0.75f, 0.55f, 0.15f, 1.0f))) {
        session.set_track_mute(i, !track.mute);
    }
    tooltip(T_("Mute"));
    ImGui::SameLine();
    if (toggle_button(T_("S"), track.solo, ImVec4(0.20f, 0.60f, 0.85f, 1.0f))) {
        session.set_track_solo(i, !track.solo);
    }
    tooltip(T_("Solo"));
    ImGui::SameLine();
    const bool armed = ui.armed && ui.armed_track == i;
    if (toggle_button(T_("R"), armed, ImVec4(0.85f, 0.15f, 0.15f, 1.0f))) {
        ui.armed = !armed;
        ui.armed_track = i;
    }
    tooltip(T_("Arm for recording: the next take goes onto this track"));

    // The remove button sits in the far corner, away from mute and solo, and asks first.
    // The track name gets the space in between and is shortened when it does not fit.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float remove_width = ImGui::CalcTextSize("X").x + 2.0f * style.FramePadding.x;
    const float remove_x = ImGui::GetWindowWidth() - remove_width - style.WindowPadding.x;

    ImGui::SameLine();
    const std::string title = std::to_string(i + 1) + ". " + track.name;
    const float title_width = remove_x - ImGui::GetCursorPosX() - style.ItemSpacing.x;
    ImGui::TextUnformatted(fit_text(title, title_width).c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) {
        ImGui::SetTooltip(T_("%s\n(click to select, double-click to rename)"), title.c_str());
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        click_track(session, i);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        ui.rename_open = true;
        ui.rename_track = i;
        std::snprintf(ui.rename_buffer, sizeof(ui.rename_buffer), "%s", track.name.c_str());
    }

    ImGui::SameLine(remove_x - group_x);
    if (ImGui::SmallButton(T_("X"))) {
        ImGui::OpenPopup("remove track");
    }
    tooltip(T_("Remove the track"));
    if (ImGui::BeginPopup("remove track")) {
        ImGui::Text(T_("Remove track %d?"), static_cast<int>(i + 1));
        if (ImGui::Button(T_("Remove"))) {
            remove = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T_("Cancel"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // The name and the value are written on the sliders, like on the master slider.
    const float slider_width = ImGui::GetContentRegionAvail().x - 0.25f * font;
    float volume = track.gain * 100.0f;
    ImGui::SetNextItemWidth(slider_width);
    if (ImGui::SliderFloat("##gain", &volume, 0.0f, 200.0f, T_("Volume: %.0f%%"),
                           ImGuiSliderFlags_AlwaysClamp)) {
        session.set_track_gain(i, volume / 100.0f);
    }
    if (double_clicked()) {
        session.set_track_gain(i, 1.0f);
    }
    tooltip(T_("Double-click: back to 100%"));

    float pan = track.pan * 100.0f;
    char pan_text[48];
    const int pan_amount = static_cast<int>(std::lround(std::fabs(pan)));
    if (pan_amount == 0) {
        std::snprintf(pan_text, sizeof(pan_text), "%s", T_("Pan: centre"));
    } else {
        // "%%%%" becomes "%%" here and a single percent sign on the slider.
        std::snprintf(pan_text, sizeof(pan_text), T_("Pan: %d%%%% %s"), pan_amount,
                      T_(pan < 0.0f ? "left" : "right"));
    }
    ImGui::SetNextItemWidth(slider_width);
    if (ImGui::SliderFloat("##pan", &pan, -100.0f, 100.0f, pan_text, ImGuiSliderFlags_AlwaysClamp)) {
        session.set_track_pan(i, std::fabs(pan) < 3.0f ? 0.0f : pan / 100.0f);  // snap to centre
    }
    if (double_clicked()) {
        session.set_track_pan(i, 0.0f);
    }
    tooltip(T_("Double-click: back to the centre"));

    draw_meter(session.track_peak(i), slider_width, 0.75f * font);
    ImGui::EndGroup();

    // The box that renames the track.
    if (ui.rename_open && ui.rename_track == i) {
        ImGui::OpenPopup("rename track");
        ui.rename_open = false;
    }
    if (ImGui::BeginPopup("rename track")) {
        ImGui::TextUnformatted(T_("Track name"));
        if (ImGui::IsWindowAppearing()) {
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(16.0f * font);
        const bool entered = ImGui::InputText(
            "##name", ui.rename_buffer, sizeof(ui.rename_buffer),
            ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        if (entered || ImGui::Button(T_("OK"))) {
            session.set_track_name(i, ui.rename_buffer);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T_("Cancel"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ui.open_track_menu = true;  // empty part of the track header
    }
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        click_track(session, i);
    }
    ImGui::EndChild();
    if (dimmed) {
        ImGui::PopStyleVar();
    }
    return remove;
}

// ---- Piano roll --------------------------------------------------------------------------

std::string pitch_name(int pitch) {
    static const char* const names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return std::string(names[pitch % 12]) + std::to_string(pitch / 12 - 1);
}

bool is_black_key(int pitch) {
    const int key = pitch % 12;
    return key == 1 || key == 3 || key == 6 || key == 8 || key == 10;
}

// One step of the grid in frames (a quarter beat when the grid is off).
double grid_step_frames(const Session& session) {
    switch (session.grid_step()) {
        case GridStep::Bar:
            return session.bar_frames();
        case GridStep::Beat:
            return session.beat_frames();
        case GridStep::HalfBeat:
            return session.beat_frames() / 2.0;
        case GridStep::QuarterBeat:
        case GridStep::None:
            break;
    }
    return session.beat_frames() / 4.0;
}

void open_piano_roll(UiState& ui, std::size_t track, std::size_t region) {
    ui.roll_open = true;
    ui.roll_track = track;
    ui.roll_region = region;
    ui.roll_source = nullptr;
    ui.roll_mode = 0;
    ui.roll_scroll = true;
}

// Puts the notes (and their selection flags) in order of start, as the session keeps them.
void sort_roll_notes(UiState& ui) {
    std::vector<std::size_t> order(ui.roll_notes.size());
    for (std::size_t k = 0; k < order.size(); ++k) {
        order[k] = k;
    }
    std::stable_sort(order.begin(), order.end(), [&ui](std::size_t a, std::size_t b) {
        return ui.roll_notes[a].start < ui.roll_notes[b].start;
    });
    std::vector<toh::core::Note> notes;
    std::vector<char> selected;
    for (const std::size_t k : order) {
        notes.push_back(ui.roll_notes[k]);
        selected.push_back(ui.roll_selected[k]);
    }
    ui.roll_notes = std::move(notes);
    ui.roll_selected = std::move(selected);
}

void draw_piano_roll(Session& session, UiState& ui) {
    ui.roll_focused = false;
    if (!ui.roll_open) {
        return;
    }
    const RegionInfo* region = nullptr;
    if (ui.roll_track < session.tracks().size() &&
        ui.roll_region < session.tracks()[ui.roll_track].regions.size()) {
        region = &session.tracks()[ui.roll_track].regions[ui.roll_region];
    }
    if (region == nullptr || !region->has_notes()) {
        ui.roll_open = false;
        return;
    }
    const ImGuiIO& io = ImGui::GetIO();
    const float font = ImGui::GetFontSize();
    ImGui::SetNextWindowSize(ImVec2(62.0f * font, 28.0f * font), ImGuiCond_FirstUseEver);
    const std::string title = std::string(T_("Piano roll")) + ": " +
                              session.tracks()[ui.roll_track].name + "###piano roll";
    if (!ImGui::Begin(title.c_str(), &ui.roll_open)) {
        ImGui::End();
        return;
    }
    ui.roll_focused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);

    // The working copy is taken again whenever the notes changed elsewhere (undo, the lane).
    const void* source_id = region->source.get();
    if (ui.roll_mode == 0 && source_id != ui.roll_source) {
        ui.roll_notes = region->source->clip->notes->notes;
        if (ui.roll_selected.size() != ui.roll_notes.size()) {
            ui.roll_selected.assign(ui.roll_notes.size(), 0);
        }
        ui.roll_source = source_id;
    }
    const std::int64_t frames = region->source->clip->frames();
    const double beat = std::max(session.beat_frames(), 1.0);
    const double step = std::max(grid_step_frames(session), 1.0);
    if (ui.roll_last_length <= 0) {
        ui.roll_last_length = std::llround(beat);
    }

    if (ImGui::Button("-##roll zoom")) {
        ui.roll_beat_px = std::max(ui.roll_beat_px / 1.5f, 8.0f);
    }
    ImGui::SameLine();
    if (ImGui::Button("+##roll zoom")) {
        ui.roll_beat_px = std::min(ui.roll_beat_px * 1.5f, 600.0f);
    }
    ImGui::SameLine();
    ImGui::AlignTextToFramePadding();
    ImGui::Text(T_("Notes: %d"), static_cast<int>(ui.roll_notes.size()));
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T_("Double-click: add a note. Drag a note: move it; its right end: length. "
                                 "Drag on an empty place: select with a box. Right-click or Delete: remove. "
                                 "Ctrl: select several. Alt: off the grid."));
    ImGui::TextDisabled("%s", T_("Wheel: zoom. Scrollbars: move. The strip on top: place the playhead. "
                                 "The keys on the left: listen."));

    const float key_h = std::max(0.85f * font, 8.0f);
    const float keys_w = 3.5f * font;
    const float ruler_h = font + 4.0f;
    // The wheel only zooms, around the mouse; moving is left to the scrollbars.
    float next_scroll_x = -1.0f;
    if (ui.roll_hovered && io.MouseWheel != 0.0f) {
        const float old_px = ui.roll_beat_px;
        ui.roll_beat_px = std::clamp(ui.roll_beat_px * std::pow(1.25f, io.MouseWheel), 8.0f, 600.0f);
        const float mouse_in_view = io.MousePos.x - ui.roll_canvas_x;
        const float content_x = ui.roll_scroll_x + mouse_in_view - keys_w;
        next_scroll_x = std::max(content_x * (ui.roll_beat_px / old_px) + keys_w - mouse_in_view, 0.0f);
    }
    const float width = keys_w + static_cast<float>(static_cast<double>(frames) / beat) * ui.roll_beat_px;
    const float height = 128.0f * key_h;
    ImGui::SetNextWindowContentSize(ImVec2(width, height));
    if (next_scroll_x >= 0.0f) {
        ImGui::SetNextWindowScroll(ImVec2(next_scroll_x, -1.0f));
    }
    ImGui::BeginChild("roll canvas", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ui.roll_hovered = ImGui::IsWindowHovered();
    ui.roll_scroll_x = ImGui::GetScrollX();
    ui.roll_canvas_x = ImGui::GetCursorScreenPos().x + ImGui::GetScrollX();
    const float ruler_bottom = ImGui::GetWindowPos().y + ruler_h;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("roll", ImVec2(width, height),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    const bool activated = ImGui::IsItemActivated();
    const bool active = ImGui::IsItemActive();
    if (ui.roll_scroll) {
        ImGui::SetScrollY((127.0f - 66.0f) * key_h - ImGui::GetWindowHeight() * 0.5f);
        ui.roll_scroll = false;
    }

    const auto frame_to_px = [&](double frame) {
        return origin.x + keys_w + static_cast<float>(frame / beat) * ui.roll_beat_px;
    };
    const auto px_to_frame = [&](float x) {
        return static_cast<double>(x - origin.x - keys_w) / static_cast<double>(ui.roll_beat_px) * beat;
    };
    const auto pitch_to_y = [&](int pitch) {
        return origin.y + static_cast<float>(127 - pitch) * key_h;
    };
    // The grid follows the timeline, so notes line up with the bars of the project.
    const auto snap = [&](double rel, bool down) -> std::int64_t {
        const std::int64_t value = std::llround(rel);
        if (io.KeyAlt || session.grid_step() == GridStep::None) {
            return value;
        }
        const double timeline = static_cast<double>(region->start - region->offset) + rel;
        const double cells = timeline / step;
        const double snapped = (down ? std::floor(cells) : std::round(cells)) * step;
        return std::llround(snapped) - region->start + region->offset;
    };

    const double mouse_frame = px_to_frame(io.MousePos.x);
    const int mouse_pitch =
        std::clamp(127 - static_cast<int>(std::floor((io.MousePos.y - origin.y) / key_h)), 0, 127);
    const bool in_ruler = io.MousePos.y < ruler_bottom;
    const bool in_grid = io.MousePos.x > origin.x + keys_w && !in_ruler;
    const double shift_frames = static_cast<double>(region->start - region->offset);  // timeline - note
    // The note that sounds while the mouse holds a note or a key (-1: none).
    const auto preview = [&](int pitch, float velocity) {
        if (pitch == ui.roll_preview) {
            return;
        }
        if (ui.roll_preview >= 0) {
            session.note_off(ui.roll_preview);
        }
        ui.roll_preview = -1;
        if (pitch >= 0 && !session.is_recording()) {
            session.set_live_track(ui.roll_track);
            session.note_on(pitch, velocity);
            ui.roll_preview = pitch;
        }
    };

    // The note under the mouse, and whether the mouse is near its right end.
    std::size_t under = ui.roll_notes.size();
    bool near_end = false;
    if (hovered && in_grid) {
        for (std::size_t k = ui.roll_notes.size(); k-- > 0;) {
            const toh::core::Note& note = ui.roll_notes[k];
            const float x0 = frame_to_px(static_cast<double>(note.start));
            const float x1 = std::max(frame_to_px(static_cast<double>(note.start + note.length)), x0 + 3.0f);
            if (note.pitch == mouse_pitch && io.MousePos.x >= x0 && io.MousePos.x <= x1) {
                under = k;
                near_end = io.MousePos.x > x1 - std::min(6.0f, (x1 - x0) * 0.4f);
                break;
            }
        }
    }
    if (ui.roll_mode == 2 || (ui.roll_mode == 0 && near_end)) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }

    bool changed = false;
    const std::int64_t shortest = std::max<std::int64_t>(std::llround(step / 4.0), 16);
    // The strip on top places the playhead; the keys on the left sound their note.
    if (activated && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && in_ruler) {
        ui.roll_mode = 3;
    } else if (activated && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !in_grid) {
        ui.roll_mode = 4;
    }
    if (ui.roll_mode == 3 && active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const double at = std::clamp(mouse_frame, 0.0, static_cast<double>(frames));
        session.seek_frames(std::max<std::int64_t>(std::llround(at + shift_frames), 0));
    }
    if (ui.roll_mode == 4 && active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        preview(mouse_pitch, 0.8f);
    }
    if (activated && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && in_grid) {
        if (under < ui.roll_notes.size()) {
            if (io.KeyCtrl) {
                ui.roll_selected[under] = ui.roll_selected[under] != 0 ? 0 : 1;
            } else if (ui.roll_selected[under] == 0) {
                std::fill(ui.roll_selected.begin(), ui.roll_selected.end(), 0);
                ui.roll_selected[under] = 1;
            }
            ui.roll_anchor = under;
            ui.roll_mode = near_end ? 2 : 1;
        } else if (!ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            // A single click on an empty place clears the selection, and dragging from there
            // draws a box that selects the notes it touches. Notes are added with a double-click.
            if (!io.KeyCtrl) {
                std::fill(ui.roll_selected.begin(), ui.roll_selected.end(), 0);
            }
            ui.roll_mode = 5;
            ui.roll_box_base = ui.roll_selected;
            ui.roll_box_y = io.MousePos.y - origin.y;
        } else {
            if (!io.KeyCtrl) {
                std::fill(ui.roll_selected.begin(), ui.roll_selected.end(), 0);
            }
            toh::core::Note note;
            note.start = std::clamp<std::int64_t>(snap(mouse_frame, true), 0, std::max<std::int64_t>(frames - 1, 0));
            note.length = std::min(ui.roll_last_length, frames - note.start);
            note.pitch = static_cast<std::uint8_t>(mouse_pitch);
            note.velocity = 0.8f;
            ui.roll_notes.push_back(note);
            ui.roll_selected.push_back(1);
            ui.roll_anchor = ui.roll_notes.size() - 1;
            ui.roll_mode = 2;  // dragging right away makes it longer
            changed = true;
        }
        ui.roll_origin = ui.roll_notes;
        ui.roll_start_frame = mouse_frame;
        ui.roll_start_pitch = mouse_pitch;
        if (ui.roll_anchor < ui.roll_notes.size()) {
            preview(ui.roll_notes[ui.roll_anchor].pitch, ui.roll_notes[ui.roll_anchor].velocity);
        }
    }

    // Dragging: the anchor note follows the grid and the other selected notes keep their
    // distance to it.
    // The selection box: from where the drag began to the mouse.
    if (ui.roll_mode == 5 && active && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        ui.roll_box_base.size() == ui.roll_notes.size()) {
        const double f0 = std::min(ui.roll_start_frame, mouse_frame);
        const double f1 = std::max(ui.roll_start_frame, mouse_frame);
        const float y_now = io.MousePos.y - origin.y;
        const int high = 127 - static_cast<int>(std::floor(std::min(ui.roll_box_y, y_now) / key_h));
        const int low = 127 - static_cast<int>(std::floor(std::max(ui.roll_box_y, y_now) / key_h));
        for (std::size_t k = 0; k < ui.roll_notes.size(); ++k) {
            const toh::core::Note& note = ui.roll_notes[k];
            const bool inside = static_cast<double>(note.start) < f1 &&
                                static_cast<double>(note.start + note.length) > f0 &&
                                note.pitch >= low && note.pitch <= high;
            ui.roll_selected[k] = (inside || ui.roll_box_base[k] != 0) ? 1 : 0;
        }
    }
    if ((ui.roll_mode == 1 || ui.roll_mode == 2) && active && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        ui.roll_anchor < ui.roll_origin.size() && ui.roll_origin.size() == ui.roll_notes.size()) {
        const double dx = mouse_frame - ui.roll_start_frame;
        const int dp = mouse_pitch - ui.roll_start_pitch;
        const toh::core::Note& anchor = ui.roll_origin[ui.roll_anchor];
        std::vector<toh::core::Note> moved = ui.roll_origin;
        if (ui.roll_mode == 1) {
            const std::int64_t delta = snap(static_cast<double>(anchor.start) + dx, false) - anchor.start;
            for (std::size_t k = 0; k < moved.size(); ++k) {
                if (ui.roll_selected[k] == 0) {
                    continue;
                }
                toh::core::Note& note = moved[k];
                note.start = std::clamp<std::int64_t>(note.start + delta, 0, std::max<std::int64_t>(frames - 1, 0));
                note.pitch = static_cast<std::uint8_t>(std::clamp(static_cast<int>(note.pitch) + dp, 0, 127));
                note.length = std::min(note.length, frames - note.start);
            }
            // The dragged note sounds again at its new pitch.
            preview(moved[ui.roll_anchor].pitch, moved[ui.roll_anchor].velocity);
        } else {
            const std::int64_t anchor_end = anchor.start + anchor.length;
            const std::int64_t delta = snap(static_cast<double>(anchor_end) + dx, false) - anchor_end;
            for (std::size_t k = 0; k < moved.size(); ++k) {
                if (ui.roll_selected[k] == 0) {
                    continue;
                }
                toh::core::Note& note = moved[k];
                note.length = std::clamp<std::int64_t>(note.length + delta, shortest,
                                                       std::max<std::int64_t>(frames - note.start, 1));
            }
            ui.roll_last_length = moved[ui.roll_anchor].length;
        }
        bool differs = false;
        for (std::size_t k = 0; k < moved.size() && !differs; ++k) {
            const toh::core::Note& a = moved[k];
            const toh::core::Note& b = ui.roll_notes[k];
            differs = a.start != b.start || a.length != b.length || a.pitch != b.pitch;
        }
        if (differs) {
            ui.roll_notes = std::move(moved);
            changed = true;
        }
    }
    if (ui.roll_mode != 0 && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        if (ui.roll_mode == 1 || ui.roll_mode == 2) {
            sort_roll_notes(ui);  // the drag is over
        }
        ui.roll_mode = 0;
    }
    if (ui.roll_mode == 0) {
        preview(-1, 0.0f);
    }

    // Removing: right-click a note, or Delete for the selected ones; Ctrl+A selects all.
    if (ui.roll_mode == 0 && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
        under < ui.roll_notes.size()) {
        ui.roll_notes.erase(ui.roll_notes.begin() + static_cast<std::ptrdiff_t>(under));
        ui.roll_selected.erase(ui.roll_selected.begin() + static_cast<std::ptrdiff_t>(under));
        changed = true;
    }
    if (ui.roll_mode == 0 && ui.roll_focused && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
            std::vector<toh::core::Note> kept;
            for (std::size_t k = 0; k < ui.roll_notes.size(); ++k) {
                if (ui.roll_selected[k] == 0) {
                    kept.push_back(ui.roll_notes[k]);
                }
            }
            if (kept.size() != ui.roll_notes.size()) {
                ui.roll_notes = std::move(kept);
                ui.roll_selected.assign(ui.roll_notes.size(), 0);
                changed = true;
            }
        }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
            std::fill(ui.roll_selected.begin(), ui.roll_selected.end(), 1);
        }
    }
    if (changed) {
        session.set_region_notes(ui.roll_track, ui.roll_region, ui.roll_notes);
        ui.roll_source = nullptr;  // taken again once nothing is dragged
        if (ui.roll_mode != 0) {
            ui.roll_source = session.tracks()[ui.roll_track].regions[ui.roll_region].source.get();
        }
    }

    // Drawing: keys and rows, the grid, the part outside the region, the notes, the playhead.
    ImDrawList* d = ImGui::GetWindowDrawList();
    const float view_left = ImGui::GetWindowPos().x;
    const float view_right = view_left + ImGui::GetWindowWidth();
    const float view_top = ImGui::GetWindowPos().y;
    const float view_bottom = view_top + ImGui::GetWindowHeight();
    for (int pitch = 0; pitch < 128; ++pitch) {
        const float y = pitch_to_y(pitch);
        if (y + key_h < view_top || y > view_bottom) {
            continue;
        }
        d->AddRectFilled(ImVec2(origin.x + keys_w, y), ImVec2(origin.x + width, y + key_h),
                         is_black_key(pitch) ? IM_COL32(29, 31, 37, 255) : IM_COL32(38, 41, 48, 255));
        d->AddRectFilled(ImVec2(origin.x, y), ImVec2(origin.x + keys_w - 1.0f, y + key_h - 1.0f),
                         is_black_key(pitch) ? IM_COL32(24, 24, 28, 255) : IM_COL32(215, 215, 222, 255));
        if (pitch % 12 == 0) {
            d->AddText(ImVec2(origin.x + 3.0f, y + (key_h - font) * 0.5f), IM_COL32(40, 40, 55, 255),
                       pitch_name(pitch).c_str());
            d->AddLine(ImVec2(origin.x + keys_w, y + key_h), ImVec2(origin.x + width, y + key_h),
                       IM_COL32(70, 74, 86, 255));
        }
    }
    // Vertical lines: steps of the grid, beats, and brighter bars.
    double line_step = step;
    while (line_step / beat * static_cast<double>(ui.roll_beat_px) < 6.0) {
        line_step *= 2.0;
    }
    const double bar = std::max(session.bar_frames(), 1.0);
    const double shift = static_cast<double>(region->start - region->offset);  // timeline - note frame
    for (double t = std::ceil(shift / line_step) * line_step; t - shift <= static_cast<double>(frames);
         t += line_step) {
        const float x = frame_to_px(t - shift);
        if (x < view_left - 1.0f) {
            continue;
        }
        if (x > view_right + 1.0f) {
            break;
        }
        const bool on_bar = std::fabs(std::remainder(t, bar)) < 0.5;
        const bool on_beat = std::fabs(std::remainder(t, beat)) < 0.5;
        d->AddLine(ImVec2(x, std::max(origin.y, view_top)), ImVec2(x, std::min(origin.y + height, view_bottom)),
                   on_bar ? IM_COL32(110, 115, 130, 255)
                          : (on_beat ? IM_COL32(70, 74, 86, 255) : IM_COL32(50, 53, 62, 255)));
    }
    // Notes before or after the region are kept but not heard.
    const ImU32 outside = IM_COL32(0, 0, 0, 110);
    if (region->offset > 0) {
        d->AddRectFilled(ImVec2(origin.x + keys_w, std::max(origin.y, view_top)),
                         ImVec2(frame_to_px(static_cast<double>(region->offset)), std::min(origin.y + height, view_bottom)),
                         outside);
    }
    if (region->offset + region->length < frames) {
        d->AddRectFilled(ImVec2(frame_to_px(static_cast<double>(region->offset + region->length)), std::max(origin.y, view_top)),
                         ImVec2(origin.x + width, std::min(origin.y + height, view_bottom)), outside);
    }
    for (std::size_t k = 0; k < ui.roll_notes.size(); ++k) {
        const toh::core::Note& note = ui.roll_notes[k];
        const float x0 = frame_to_px(static_cast<double>(note.start));
        const float x1 = std::max(frame_to_px(static_cast<double>(note.start + note.length)), x0 + 3.0f);
        const float y = pitch_to_y(note.pitch);
        if (x1 < view_left || x0 > view_right || y + key_h < view_top || y > view_bottom) {
            continue;
        }
        const bool selected = k < ui.roll_selected.size() && ui.roll_selected[k] != 0;
        d->AddRectFilled(ImVec2(x0, y + 1.0f), ImVec2(x1, y + key_h - 1.0f),
                         selected ? IM_COL32(255, 225, 160, 255) : IM_COL32(240, 160, 80, 255), 2.0f);
        d->AddRect(ImVec2(x0, y + 1.0f), ImVec2(x1, y + key_h - 1.0f), IM_COL32(60, 35, 10, 255), 2.0f);
    }
    if (ui.roll_mode == 5 && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 2.0f)) {
        const ImVec2 a(frame_to_px(ui.roll_start_frame), origin.y + ui.roll_box_y);
        const ImVec2 b = io.MousePos;
        const ImVec2 lo(std::min(a.x, b.x), std::min(a.y, b.y));
        const ImVec2 hi(std::max(a.x, b.x), std::max(a.y, b.y));
        d->AddRectFilled(lo, hi, IM_COL32(150, 190, 255, 45));
        d->AddRect(lo, hi, IM_COL32(170, 205, 255, 220));
    }
    // The strip on top, always in view: bar numbers; click or drag in it to place the playhead.
    const float strip_left = std::max(origin.x + keys_w, view_left);
    d->AddRectFilled(ImVec2(view_left, view_top), ImVec2(view_right, ruler_bottom),
                     in_ruler && hovered ? IM_COL32(48, 52, 64, 255) : IM_COL32(36, 39, 48, 255));
    d->AddLine(ImVec2(view_left, ruler_bottom), ImVec2(view_right, ruler_bottom), IM_COL32(90, 95, 110, 255));
    {
        double bar_step = bar;
        while (bar_step / beat * static_cast<double>(ui.roll_beat_px) < 3.0f * font) {
            bar_step *= 2.0;
        }
        for (double t = std::ceil(shift / bar_step) * bar_step; t - shift <= static_cast<double>(frames);
             t += bar_step) {
            const float x = frame_to_px(t - shift);
            if (x < strip_left - 1.0f) {
                continue;
            }
            if (x > view_right + 1.0f) {
                break;
            }
            d->AddLine(ImVec2(x, ruler_bottom - 5.0f), ImVec2(x, ruler_bottom), IM_COL32(150, 155, 170, 255));
            const std::string number = std::to_string(static_cast<long long>(std::llround(t / bar)) + 1);
            d->AddText(ImVec2(x + 3.0f, view_top + 2.0f), IM_COL32(190, 195, 210, 255), number.c_str());
        }
    }
    const double playhead = static_cast<double>(session.position_frames()) - shift;
    if (playhead >= 0.0 && playhead <= static_cast<double>(frames)) {
        const float x = frame_to_px(playhead);
        d->AddLine(ImVec2(x, std::max(origin.y, view_top)), ImVec2(x, std::min(origin.y + height, view_bottom)),
                   IM_COL32(255, 80, 70, 230), 2.0f);
        if (x >= strip_left) {
            d->AddTriangleFilled(ImVec2(x - 6.0f, view_top), ImVec2(x + 6.0f, view_top),
                                 ImVec2(x, ruler_bottom), IM_COL32(255, 80, 70, 255));
        }
    }
    ImGui::EndChild();
    ImGui::End();
}

void draw_tracks(Session& session, UiState& ui) {
    View& view = ui.view;
    const ImGuiIO& io = ImGui::GetIO();
    const float font = ImGui::GetFontSize();
    const float header_width = 22.5f * font;
    const float row_height = 6.2f * font;

    // Mouse wheel over the timeline: Ctrl zooms around the mouse, Shift scrolls sideways.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        // With no region selected the plain wheel zooms too; with a selection it scrolls the
        // track list (the "tracks" child window is created without wheel scrolling otherwise).
        const bool plain_zoom = !ui.selected && !io.KeyShift;
        if ((io.KeyCtrl || plain_zoom) && io.MouseWheel != 0.0f) {
            zoom_view(view, std::pow(1.25, static_cast<double>(io.MouseWheel)),
                      std::max(io.MousePos.x, view.lane_origin_x));
        } else if (io.KeyShift && io.MouseWheel != 0.0f) {
            view.start_seconds -= static_cast<double>(io.MouseWheel) * 80.0 / view.pixels_per_second;
        }
        if (io.MouseWheelH != 0.0f) {
            view.start_seconds -= static_cast<double>(io.MouseWheelH) * 80.0 / view.pixels_per_second;
        }
        view.start_seconds = std::max(view.start_seconds, 0.0);
    }

    // Show everything when the first tracks arrive.
    if (session.tracks().size() != ui.known_tracks) {
        if (ui.known_tracks == 0) {
            view.fit_pending = true;
        }
        ui.known_tracks = session.tracks().size();
    }

    draw_ruler(session, ui, header_width);  // also measures where the lanes are

    if (view.fit_pending && session.length_frames() > 0) {
        fit_view(session, view);
        view.fit_pending = false;
    }

    // Keep the playhead in view while playing.
    if (ui.follow && session.is_playing() && ui.drag == DragKind::None) {
        const double visible = static_cast<double>(view.lane_width) / view.pixels_per_second;
        const double position = static_cast<double>(session.position_frames()) /
                                static_cast<double>(session.sample_rate());
        if (position > view.start_seconds + visible) {
            view.start_seconds = position - visible * 0.05;
        } else if (position < view.start_seconds) {
            view.start_seconds = std::max(0.0, position - visible * 0.95);
        }
    }

    // While something is dragged, holding the mouse at or past the left or right edge of the
    // lanes scrolls the timeline: the further out, the faster.
    if (ui.drag != DragKind::None) {
        const float margin = font;
        const float left = view.lane_origin_x + margin;
        const float right = view.lane_origin_x + view.lane_width - margin;
        float push = 0.0f;
        if (io.MousePos.x > right) {
            push = io.MousePos.x - right;
        } else if (io.MousePos.x < left) {
            push = io.MousePos.x - left;
        }
        if (push != 0.0f) {
            push = std::clamp(push, -300.0f, 300.0f);
            const double pixels = static_cast<double>(push) * 12.0 * static_cast<double>(io.DeltaTime);
            view.start_seconds = std::max(0.0, view.start_seconds + pixels / view.pixels_per_second);
        }
    }

    std::size_t remove_index = session.tracks().size();
    for (std::size_t i = 0; i < session.tracks().size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (draw_track_header(session, ui, i, header_width, row_height)) {
            remove_index = i;
        }
        ImGui::SameLine();
        draw_lane(session, ui, i, row_height);
        ImGui::PopID();
    }

    if (remove_index < session.tracks().size()) {
        session.remove_track(remove_index);
        ui.selected = false;
        ui.armed = false;
        ui.drag = DragKind::None;
    }

    // Right-click on empty space (below the tracks, an empty part of a lane or of a track
    // header): a menu to add tracks.
    if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        ui.open_track_menu = true;
    }
    if (ui.open_track_menu) {
        ImGui::OpenPopup("track area menu");
        ui.open_track_menu = false;
    }
    if (ImGui::BeginPopup("track area menu")) {
        if (ImGui::MenuItem(T_("New track"), key_label(ui, Action::NewTrack))) {
            add_new_track(session, ui, false);
        }
        if (ImGui::MenuItem(T_("New track for recording"))) {
            add_new_track(session, ui, true);
        }
        if (ImGui::BeginMenu(T_("New instrument track"))) {
            int program = -2;
            toh::core::Instrument instrument = toh::core::Instrument::Keys;
            if (instrument_menu(session, -2, toh::core::Instrument::Keys, program, instrument)) {
                add_instrument_track(session, ui, instrument, program);
            }
            ImGui::EndMenu();
        }
        draw_playback_menu(session, ui);
        ImGui::EndPopup();
    }

    // A click on empty space anywhere in the track area (below the tracks, or on the free part
    // of a track header) clears the selection. Clicks on controls and regions do not.
    if (ui.selected && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
        !ImGui::IsAnyItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ui.selected = false;
    }

    if (session.tracks().empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("%s", T_("Drop audio files here (WAV, FLAC, MP3) or press \"Add files...\"."));
        ImGui::TextDisabled("%s", T_("To record an instrument or a voice, add a track for recording:"));
        if (ImGui::Button(T_("New track for recording"))) {
            add_new_track(session, ui, true);
        }
    }
}

// A scrollbar for the timeline, under the lanes: the thumb shows which part is visible.
void draw_timeline_scrollbar(const Session& session, UiState& ui) {
    View& view = ui.view;
    const ImGuiIO& io = ImGui::GetIO();
    const float font = ImGui::GetFontSize();
    const float height = 0.8f * font;
    const float width = std::max(view.lane_width, 1.0f);

    ImGui::SetCursorScreenPos(ImVec2(view.lane_origin_x, ImGui::GetCursorScreenPos().y));
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("timeline scrollbar", ImVec2(width, height));

    const double visible = static_cast<double>(width) / view.pixels_per_second;
    const double length = static_cast<double>(session.length_frames()) /
                          static_cast<double>(session.sample_rate());
    // The scrollable range: the project with a little room after it, or more when the view
    // has already been moved further.
    const double total = std::max({length * 1.05, view.start_seconds + visible, 1.0});
    const double max_start = std::max(total - visible, 0.0);
    const float thumb_width =
        std::clamp(width * static_cast<float>(visible / total), std::min(2.0f * font, width), width);
    const float travel = width - thumb_width;
    float thumb_x = origin.x;
    if (max_start > 0.0) {
        thumb_x += travel * static_cast<float>(std::min(view.start_seconds / max_start, 1.0));
    }

    if (ImGui::IsItemActivated()) {
        const bool on_thumb = io.MousePos.x >= thumb_x && io.MousePos.x <= thumb_x + thumb_width;
        ui.scroll_grab = on_thumb ? io.MousePos.x - thumb_x : thumb_width * 0.5f;  // else: jump there
    }
    if (ImGui::IsItemActive() && travel > 0.0f && max_start > 0.0) {
        const float wanted = io.MousePos.x - ui.scroll_grab - origin.x;
        view.start_seconds =
            static_cast<double>(std::clamp(wanted / travel, 0.0f, 1.0f)) * max_start;
        thumb_x = origin.x + std::clamp(wanted, 0.0f, travel);
    }

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                             IM_COL32(18, 19, 22, 255), height * 0.5f);
    const ImU32 colour = ImGui::IsItemActive()    ? IM_COL32(150, 165, 190, 255)
                         : ImGui::IsItemHovered() ? IM_COL32(120, 132, 155, 255)
                                                  : IM_COL32(92, 100, 118, 255);
    draw_list->AddRectFilled(ImVec2(thumb_x, origin.y + 1.0f),
                             ImVec2(thumb_x + thumb_width, origin.y + height - 1.0f), colour,
                             height * 0.5f);
    tooltip(T_("Drag to move along the timeline"));
}

// The line at the bottom of the window: figures about the project, the audio and the build.
void draw_status_bar(const Session& session, const UiState& ui) {
    ImGui::Separator();

    std::size_t regions = 0;
    for (const TrackInfo& track : session.tracks()) {
        regions += track.regions.size();
    }
    const toh::audio::DeviceConfig& config = session.config();
    const double buffer_ms =
        1000.0 * static_cast<double>(config.buffer_frames) / static_cast<double>(config.sample_rate);

    ImGui::AlignTextToFramePadding();
    if (session.is_recording()) {
        ImGui::TextColored(ImVec4(0.95f, 0.30f, 0.30f, 1.0f), T_("REC %s"),
                           format_time(session.recorded_frames(), session.sample_rate()).c_str());
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T_("|"));
        ImGui::SameLine();
    }
    ImGui::Text(T_("Tracks: %d   Regions: %d   Length: %s"), static_cast<int>(session.tracks().size()),
                static_cast<int>(regions),
                format_time(session.length_frames(), session.sample_rate()).c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T_("|"));
    ImGui::SameLine();
    ImGui::Text(T_("Audio: %s, %u Hz, buffer %u (%.1f ms)"), T_(session.audio_running() ? "on" : "off"),
                config.sample_rate, config.buffer_frames, buffer_ms);
    ImGui::SameLine();
    ImGui::TextDisabled("%s", T_("|"));
    ImGui::SameLine();
    ImGui::Text(T_("Zoom: %.0f px/s   %.0f fps"), ui.view.pixels_per_second,
                static_cast<double>(ImGui::GetIO().Framerate));

    if (const RegionInfo* region = selected_region(session, ui)) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", T_("|"));
        ImGui::SameLine();
        ImGui::Text(T_("Selected: track %d, %s - %s"), static_cast<int>(ui.selected_track + 1),
                    format_time(region->start, session.sample_rate()).c_str(),
                    format_time(region->end(), session.sample_rate()).c_str());
    }

    // The build, at the right edge.
    const toh::app::BuildInfo& build = toh::app::build_info();
    char text[96];
    std::snprintf(text, sizeof(text), T_("Build %s (%s, %s)"), build.version, build.commit, build.date);
    const float x = ImGui::GetWindowWidth() - ImGui::CalcTextSize(text).x -
                    ImGui::GetStyle().WindowPadding.x;
    ImGui::SameLine(std::max(x, ImGui::GetCursorPosX()));
    ImGui::TextDisabled("%s", text);
}

void load_font(float scale) {
    static const char* const candidates[] = {
        "C:\\Windows\\Fonts\\segoeui.ttf",
        "C:\\Windows\\Fonts\\arial.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    };
    ImGuiIO& io = ImGui::GetIO();
    for (const char* path : candidates) {
        std::error_code error;
        if (std::filesystem::exists(path, error) &&
            io.Fonts->AddFontFromFileTTF(path, 16.0f * scale) != nullptr) {
            return;
        }
    }
    // No system font found: ImGui falls back to its built-in font (Latin letters only).
}

}  // namespace

int main(int argc, char* argv[]) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tone of Hell", SDL_GetError(), nullptr);
        return 1;
    }

    SDL_Window* window = SDL_CreateWindow("Tone of Hell", 1280, 720,
                                          SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    SDL_Renderer* renderer = window != nullptr ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (window == nullptr || renderer == nullptr) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Tone of Hell", SDL_GetError(), nullptr);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    // Keyboard navigation of widgets stays off: the keys belong to the user's shortcuts.
    io.IniFilename = nullptr;  // no settings file next to the program yet
    ImGui::StyleColorsDark();

    float scale = SDL_GetWindowDisplayScale(window);
    if (!(scale > 0.0f)) {
        scale = 1.0f;
    }
    ImGui::GetStyle().ScaleAllSizes(scale);
    load_font(scale);

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);

    {
        Session session;
        UiState ui;
        PendingFiles pending;
        PendingExport exports;

        // Load what was remembered from the last run and apply it before the audio starts.
        if (char* folder = SDL_GetPrefPath("", "Tone of Hell")) {
            ui.settings_path = std::filesystem::u8path(folder) / "settings.txt";
            SDL_free(folder);
            ui.settings.load(ui.settings_path);
        }
        ui.window = window;
        ui.renderer = renderer;
        if (ui.settings.start_window == 2) {
            SDL_SetWindowFullscreen(window, true);
        } else if (ui.settings.start_window == 1) {
            SDL_MaximizeWindow(window);
        }
        toh::app::set_language(ui.settings.language);
        session.set_devices(ui.settings.output_device, ui.settings.input_device);
        session.set_master_gain(ui.settings.master_gain);
        session.set_input_gain(ui.settings.input_gain);
        session.set_play_selected_only(ui.settings.play_selected_only);
        session.set_reverse_starts_playback(ui.settings.reverse_starts_playback);
        session.set_reverse_off_at_start(ui.settings.reverse_off_at_start);
        session.set_metronome_endless(ui.settings.metronome_endless);
        load_soundfont(session, ui,
                       ui.settings.soundfont_path.empty()
                           ? default_soundfont_path()
                           : std::filesystem::u8path(ui.settings.soundfont_path));
        if (ui.settings.record_offset_frames >= 0) {
            session.set_record_offset_frames(ui.settings.record_offset_frames);
        }

        // The log of this session, next to the settings file.
        if (!ui.settings_path.empty()) {
            ui.logs_folder = ui.settings_path.parent_path() / "logs";
            if (ui.settings.write_logs) {
                toh::app::logging::start(ui.logs_folder);
            }
        }
        toh::app::logging::install_crash_handler();
        ui.key_pitch.fill(-1);
        ui.midi.open_all();
        for (const std::string& device : ui.midi.devices()) {
            toh::app::logging::info("MIDI keyboard: " + device);
        }
        // A recovery file that is still there means the last session did not end properly.
        if (!ui.settings_path.empty()) {
            ui.recovery_file = ui.settings_path.parent_path() / "recovery" / "recovery.tohp";
            std::error_code code;
            ui.ask_recovery = std::filesystem::exists(ui.recovery_file, code);
            if (ui.ask_recovery) {
                toh::app::logging::warning("A recovery file was found: the last session ended early");
            }
        }
        {
            const toh::app::BuildInfo& build = toh::app::build_info();
            toh::app::logging::info(std::string("Tone of Hell ") + build.version + " (commit " +
                                    build.commit + ", built " + build.date + ") on " +
                                    SDL_GetPlatform());
            toh::app::logging::info("Settings: " + ui.settings_path.u8string());
            toh::app::logging::info(
                session.soundfont()
                    ? "SoundFont: " + session.soundfont()->path().u8string() + " (" +
                          std::to_string(session.soundfont()->presets().size()) + " presets)"
                    : std::string("No SoundFont: instrument tracks use the simple synthesizers"));
        }

        std::string audio_error;
        if (!session.start_audio(&audio_error)) {
            ui.error = audio_error;
        } else {
            const toh::audio::DeviceConfig& config = session.config();
            const std::string output = session.output_device();
            toh::app::logging::info("Audio started: output \"" +
                                    (output.empty() ? std::string("system default") : output) + "\", " +
                                    std::to_string(config.sample_rate) + " Hz, " +
                                    std::to_string(config.channels) + " channels, buffer " +
                                    std::to_string(config.buffer_frames) + " frames");
        }
        for (int i = 1; i < argc; ++i) {
            pending.add(argv[i]);  // SDL passes the arguments as UTF-8
        }

        bool running = true;
        while (running) {
            // Sleep while nothing moves on screen; redraw continuously while sound plays.
            const bool animating = session.is_playing() || session.master_peak() > 0.0005f ||
                                   ui.drag != DragKind::None || session.is_recording();
            SDL_Event event;
            bool have_event = animating ? SDL_PollEvent(&event) : SDL_WaitEventTimeout(&event, 250);
            while (have_event) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) {
                    unless_unsaved(session, ui, UiState::Then::Quit);
                } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                           event.window.windowID == SDL_GetWindowID(window)) {
                    unless_unsaved(session, ui, UiState::Then::Quit);
                } else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr) {
                    if (is_picture_file(event.drop.data)) {
                        ui.dropped_picture = event.drop.data;
                        ui.dropped_y = event.drop.y;
                    } else if (is_midi_file(event.drop.data)) {
                        import_midi_file(session, ui, event.drop.data);
                    } else {
                        pending.add(event.drop.data);
                    }
                }
                have_event = SDL_PollEvent(&event);
            }

            for (const std::string& path : pending.take()) {
                std::string error;
                if (!session.add_track_from_file(std::filesystem::u8path(path), &error)) {
                    ui.error = path + ": " + error;
                }
            }
            // A file name arrived from the export dialog: write the file.
            {
                std::string export_path;
                toh::audio::SampleFormat export_format = toh::audio::SampleFormat::Int24;
                int export_kbps = -1;
                {
                    const std::lock_guard<std::mutex> lock(exports.mutex);
                    if (exports.ready) {
                        export_path = exports.path;
                        export_format = exports.format;
                        export_kbps = exports.mp3_kbps;
                        exports.ready = false;
                    }
                }
                if (!export_path.empty()) {
                    std::string ending = export_path.size() >= 4
                                             ? export_path.substr(export_path.size() - 4)
                                             : std::string();
                    for (char& character : ending) {
                        if (character >= 'A' && character <= 'Z') {
                            character = static_cast<char>(character - 'A' + 'a');
                        }
                    }
                    const char* const extension = export_kbps >= 0 ? ".mp3" : ".wav";
                    if (ending != extension) {
                        export_path += extension;
                    }
                    std::string error;
                    const auto export_file = std::filesystem::u8path(export_path);
                    const bool exported =
                        export_kbps >= 0 ? session.export_mp3(export_file, export_kbps, &error)
                                         : session.export_wav(export_file, export_format, &error);
                    if (exported) {
                        ui.notice = std::string(T_("Exported: ")) + export_path;
                    } else {
                        ui.error = std::string(T_("Export: ")) + error;
                    }
                }
            }
            handle_path_request(session, ui);
            apply_dropped_picture(session, ui);
            autosave_tick(session, ui);
            update_window_title(session, ui);
            log_events(session, ui);
            // A picture file arrived from the "track picture" dialog.
            {
                const std::lock_guard<std::mutex> lock(ui.icon_request.mutex);
                if (ui.icon_request.ready) {
                    session.set_track_icon(ui.icon_request.track, "file:" + ui.icon_request.path);
                    ui.icon_request.ready = false;
                }
            }
            session.update();

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            handle_shortcuts(session, ui, pending, window);
            play_live_notes(session, ui);

            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::Begin("main", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar);
            draw_menu_bar(session, pending, exports, window, ui);
            draw_unsaved_popup(session, ui);
            draw_recovery_popup(session, ui);
            draw_toolbar(session, pending, window, ui);
            ImGui::Separator();
            // The track area leaves room below for the timeline scrollbar and the status bar.
            const ImGuiStyle& style = ImGui::GetStyle();
            const float below = 0.8f * ImGui::GetFontSize() + ImGui::GetFrameHeight() +
                                3.0f * style.ItemSpacing.y + 2.0f;
            ImGui::BeginChild("tracks", ImVec2(0.0f, -below), ImGuiChildFlags_None,
                              ui.selected ? ImGuiWindowFlags_None
                                          : ImGuiWindowFlags_NoScrollWithMouse);
            draw_tracks(session, ui);
            ImGui::EndChild();
            draw_timeline_scrollbar(session, ui);
            draw_status_bar(session, ui);
            draw_dialogs(ui);
            draw_settings_window(session, ui);
            draw_piano_roll(session, ui);
            if (ui.settings_changed) {
                save_settings(session, ui);
            }
            // A gesture has ended once nothing is held: its edits become one undo step.
            if (ui.drag == DragKind::None && !ImGui::IsAnyItemActive() &&
                !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                session.commit_edits();
            }
            if (ui.quit) {
                running = false;
            }
            ImGui::End();

            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 20, 21, 24, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
        }

        save_settings(session, ui);
        toh::app::logging::info("The program closes normally");
        if (!ui.recovery_file.empty() && !ui.ask_recovery) {
            // Closing normally: the changes were saved or deliberately not saved.
            std::error_code code;
            std::filesystem::remove(ui.recovery_file, code);
        }
        for (const auto& entry : ui.textures) {
            if (entry.second != nullptr) {
                SDL_DestroyTexture(entry.second);
            }
        }
    }  // the session (and the audio device) is closed here, before the window goes away
    toh::app::logging::stop();

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
