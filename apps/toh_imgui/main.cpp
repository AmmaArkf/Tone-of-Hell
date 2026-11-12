// toh_imgui: the Dear ImGui user interface of Tone of Hell.
//
// This file only draws the shared toh::app::Session and forwards user actions to it. It must
// not contain audio or project logic, so that other user interfaces stay equivalent.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

#include "toh/app/session.h"
#include "toh/core/clip.h"

namespace {

using toh::app::GridStep;
using toh::app::RegionInfo;
using toh::app::Session;
using toh::app::TrackInfo;

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

// Which part of the timeline is visible.
struct View {
    double pixels_per_second = 100.0;
    double start_seconds = 0.0;
    bool fit_pending = true;     // show the whole project as soon as there is something to show
    float lane_origin_x = 0.0f;  // screen position and width of the lanes, from the last frame
    float lane_width = 800.0f;
};

enum class DragKind { None, Seek, Move, TrimStart, TrimEnd, FadeIn, FadeOut };

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
    std::size_t drag_track = 0;
    std::size_t drag_region = 0;
    std::int64_t drag_grab = 0;    // distance from the region start to the grabbed frame
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
            ImGui::SetTooltip("Level meter: %.1f dB", static_cast<double>(20.0f * std::log10(peak)));
        } else {
            ImGui::SetTooltip("Level meter (silent). The marks are at -48, -36, -24, -12 and -6 dB.");
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

void remove_selected_region(Session& session, UiState& ui) {
    if (selected_region(session, ui) != nullptr) {
        session.remove_region(ui.selected_track, ui.selected_region);
        ui.selected = false;
    }
}

// ---- Toolbar -----------------------------------------------------------------------------

// The metronome button, the tempo field and the settings popup.
void draw_metronome(Session& session, UiState& ui) {
    toh::app::MetronomeSettings settings = session.metronome();
    bool changed = false;
    const float font = ImGui::GetFontSize();

    ImGui::SameLine();
    if (toggle_button("Metronome", settings.enabled, ImVec4(0.55f, 0.35f, 0.80f, 1.0f), false)) {
        settings.enabled = !settings.enabled;
        changed = true;
    }
    tooltip("Click on every beat while playing. Also plays when no file is loaded.");

    ImGui::SameLine();
    auto bpm = static_cast<float>(settings.bpm);
    ImGui::SetNextItemWidth(6.0f * font);
    if (ImGui::DragFloat("##bpm", &bpm, 0.5f, 20.0f, 400.0f, "%.1f BPM",
                         ImGuiSliderFlags_AlwaysClamp)) {
        settings.bpm = static_cast<double>(bpm);
        changed = true;
    }
    tooltip("Tempo of the metronome and the grid. Drag, or double-click to type a value.");

    ImGui::SameLine();
    if (ImGui::Button("...##metronome")) {
        ImGui::OpenPopup("metronome settings");
    }
    tooltip("Metronome and time signature settings");

    if (ImGui::BeginPopup("metronome settings")) {
        ImGui::TextUnformatted("Metronome and grid");
        ImGui::Separator();

        int beats = settings.beats_per_bar;
        ImGui::SetNextItemWidth(7.0f * font);
        if (ImGui::InputInt("Beats per bar", &beats)) {
            settings.beats_per_bar = std::clamp(beats, 1, 32);
            changed = true;
        }

        static const int units[] = {2, 4, 8, 16};
        ImGui::SetNextItemWidth(7.0f * font);
        if (ImGui::BeginCombo("Beat unit", std::to_string(settings.beat_unit).c_str())) {
            for (const int unit : units) {
                if (ImGui::Selectable(std::to_string(unit).c_str(), unit == settings.beat_unit)) {
                    settings.beat_unit = unit;
                    changed = true;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::TextDisabled("Time signature: %d/%d. The first beat of a bar sounds higher.",
                            settings.beats_per_bar, settings.beat_unit);

        ImGui::SetNextItemWidth(7.0f * font);
        float volume = settings.volume * 100.0f;
        if (ImGui::SliderFloat("##click volume", &volume, 0.0f, 200.0f, "Click volume: %.0f%%",
                               ImGuiSliderFlags_AlwaysClamp)) {
            settings.volume = volume / 100.0f;
            changed = true;
        }

        if (ImGui::Button("Tap tempo")) {
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
        ImGui::TextDisabled("Click in time with the music");
        ImGui::EndPopup();
    }

    if (changed) {
        session.set_metronome(settings);
    }
}

void draw_toolbar(Session& session, PendingFiles& pending, SDL_Window* window, UiState& ui) {
    const float font = ImGui::GetFontSize();

    // Row 1: transport, metronome, master.
    if (ImGui::Button(session.is_playing() ? "Pause" : "Play")) {
        session.toggle_play();
    }
    tooltip("Space");
    ImGui::SameLine();
    if (ImGui::Button("Stop")) {
        session.stop();
    }
    ImGui::SameLine();
    if (toggle_button("Reverse", session.reverse(), ImVec4(0.80f, 0.45f, 0.20f, 1.0f), false)) {
        session.toggle_reverse();
    }
    tooltip("Play backwards from the playhead. Press again to play forwards.");
    ImGui::SameLine();
    bool loop = session.loop();
    if (ImGui::Checkbox("Loop", &loop)) {
        session.set_loop(loop);
    }

    ImGui::SameLine();
    ImGui::Text("%s / %s", format_time(session.position_frames(), session.sample_rate()).c_str(),
                format_time(session.length_frames(), session.sample_rate()).c_str());

    draw_metronome(session, ui);

    // The name and the value are written on the slider itself: "Master: 83%".
    ImGui::SameLine();
    float percent = session.master_gain() * 100.0f;
    ImGui::SetNextItemWidth(11.0f * font);
    if (ImGui::SliderFloat("##master", &percent, 0.0f, 200.0f, "Master: %.0f%%",
                           ImGuiSliderFlags_AlwaysClamp)) {
        session.set_master_gain(percent / 100.0f);
    }
    tooltip("Master volume. 100% leaves the mix unchanged. Ctrl+click to type a value.");
    ImGui::SameLine();
    draw_meter(session.master_peak(), 8.0f * font, ImGui::GetFrameHeight());

    const std::uint64_t clips = session.master_clip_count();
    if (clips > ui.seen_clips) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.15f, 0.15f, 1.0f));
        if (ImGui::SmallButton("CLIP")) {
            ui.seen_clips = clips;  // click to reset the indicator
        }
        ImGui::PopStyleColor();
        tooltip("The mix went above full scale. Lower the faders. Click to reset.");
    }

    // Row 2: files, grid, zoom.
    if (ImGui::Button("Add files...")) {
        static const SDL_DialogFileFilter filters[] = {{"Audio files", "wav;flac;mp3"},
                                                       {"All files", "*"}};
        SDL_ShowOpenFileDialog(&file_dialog_callback, &pending, window, filters, 2, nullptr, true);
    }
    tooltip("Every file becomes a new track. You can also drop files onto the window.");

    ImGui::SameLine();
    bool snap = session.snap();
    if (ImGui::Checkbox("Snap to", &snap)) {
        session.set_snap(snap);
    }
    tooltip("Moved and trimmed regions jump to the grid. Hold Alt while dragging to switch it off.");
    ImGui::SameLine();
    static const char* const step_names[] = {"bars", "beats", "1/2 beats", "1/4 beats"};
    const int current_step = static_cast<int>(session.grid_step());
    ImGui::SetNextItemWidth(6.5f * font);
    if (ImGui::BeginCombo("##grid", step_names[current_step])) {
        for (int i = 0; i < 4; ++i) {
            if (ImGui::Selectable(step_names[i], i == current_step)) {
                session.set_grid_step(static_cast<GridStep>(i));
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    const float centre_x = ui.view.lane_origin_x + ui.view.lane_width * 0.5f;
    if (ImGui::Button("-##zoom")) {
        zoom_view(ui.view, 1.0 / 1.5, centre_x);
    }
    tooltip("Zoom out (Ctrl + mouse wheel)");
    ImGui::SameLine();
    if (ImGui::Button("+##zoom")) {
        zoom_view(ui.view, 1.5, centre_x);
    }
    tooltip("Zoom in (Ctrl + mouse wheel)");
    ImGui::SameLine();
    if (ImGui::Button("Fit")) {
        fit_view(session, ui.view);
    }
    tooltip("Show the whole project");
    ImGui::SameLine();
    ImGui::Checkbox("Follow", &ui.follow);
    tooltip("Keep the playhead in view while playing");

    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Drag a region to move it and its left or right edge to trim it.\n"
            "Drag the small squares in its top corners to make fades.\n"
            "S cuts at the playhead, Delete removes the selected region; right-click for a menu.\n"
            "Ctrl + wheel zooms, Shift + wheel scrolls, Space plays, Home returns to the start.");
    }

    if (!session.audio_running()) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.25f, 1.0f), "No audio device: playback is silent.");
    }
    if (!ui.error.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.35f, 1.0f), "%s", ui.error.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Dismiss")) {
            ui.error.clear();
        }
    }
}

// ---- Timeline ----------------------------------------------------------------------------

// Vertical lines at bars and beats. With `labels`, bar numbers are written next to the lines.
void draw_grid(const Session& session, const View& view, ImDrawList* draw_list, float top,
               float bottom, bool labels) {
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
    const float height = 2.5f * font;
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
        draw_list->AddLine(ImVec2(x, origin.y + height * 0.75f), ImVec2(x, bottom),
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
        draw_list->AddText(ImVec2(x + 3.0f, origin.y + height * 0.5f - 1.0f),
                           IM_COL32(170, 170, 180, 255), label);
    }
    draw_playhead(session, view, draw_list, origin.y, bottom);
    draw_list->PopClipRect();
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
        } else {
            hit.kind = DragKind::Move;
        }
        return hit;
    }
    return Hit();
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
    draw_list->AddRectFilled(low, high, selected ? IM_COL32(58, 84, 116, 255) : IM_COL32(44, 62, 84, 255),
                             3.0f);

    // Waveform: one vertical line per pixel column, only for the visible part.
    const ImU32 wave = track.mute ? IM_COL32(110, 120, 130, 255) : IM_COL32(120, 190, 255, 255);
    const float middle = top + height * 0.5f;
    const float half = height * 0.5f - 3.0f;
    const int first_column = static_cast<int>(std::floor(std::max(x0, left_edge)));
    const int last_column = static_cast<int>(std::ceil(std::min(x1, right_edge)));
    const std::int64_t source_first = region.offset;
    const std::int64_t source_last = region.offset + region.length - 1;
    for (int column = first_column; column < last_column; ++column) {
        std::int64_t first = x_to_frame(session, view, static_cast<float>(column)) - region.start +
                             region.offset;
        std::int64_t last = x_to_frame(session, view, static_cast<float>(column + 1)) - 1 -
                            region.start + region.offset;
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

    // Fades: the part that is turned down is shaded, with a line along the fade.
    const float fade_in_x = frame_to_x(session, view, region.start + region.fade_in);
    const float fade_out_x = frame_to_x(session, view, region.end() - region.fade_out);
    const ImU32 shade = IM_COL32(10, 12, 16, 150);
    const ImU32 fade_line = IM_COL32(235, 235, 245, 200);
    if (region.fade_in > 0) {
        draw_list->AddTriangleFilled(ImVec2(x0, low.y), ImVec2(fade_in_x, low.y), ImVec2(x0, high.y),
                                     shade);
        draw_list->AddLine(ImVec2(x0, high.y), ImVec2(fade_in_x, low.y), fade_line);
    }
    if (region.fade_out > 0) {
        draw_list->AddTriangleFilled(ImVec2(x1, low.y), ImVec2(fade_out_x, low.y), ImVec2(x1, high.y),
                                     shade);
        draw_list->AddLine(ImVec2(fade_out_x, low.y), ImVec2(x1, high.y), fade_line);
    }

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
                           fit_text(region.source->name, room).c_str());
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
    const bool dragging_here = ui.drag != DragKind::None && ui.drag_track == track_index;

    Hit hit;
    if (hovered && ui.drag == DragKind::None) {
        hit = hit_test(session, view, track, io.MousePos, origin.y);
        if (hit.kind == DragKind::TrimStart || hit.kind == DragKind::TrimEnd) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        } else if (hit.kind == DragKind::FadeIn || hit.kind == DragKind::FadeOut) {
            ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        }
    }

    // Start of a click or drag.
    if (ImGui::IsItemActivated()) {
        ui.drag_track = track_index;
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
        }
    }

    // Right-click: select the region and open its menu.
    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right) && hit.kind != DragKind::None) {
        ui.selected = true;
        ui.selected_track = track_index;
        ui.selected_region = hit.region;
        ImGui::OpenPopup("region menu");
    }
    if (ImGui::BeginPopup("region menu")) {
        const RegionInfo* region = selected_region(session, ui);
        const std::int64_t playhead = session.position_frames();
        const bool can_split =
            region != nullptr && playhead > region->start && playhead < region->end();
        if (ImGui::MenuItem("Split at the playhead", "S", false, can_split)) {
            split_at_playhead(session, ui);
        }
        if (ImGui::MenuItem("Remove fades", nullptr, false,
                            region != nullptr && (region->fade_in > 0 || region->fade_out > 0))) {
            session.set_region_fades(ui.selected_track, ui.selected_region, 0, 0);
        }
        if (ImGui::MenuItem("Remove the region", "Delete", false, region != nullptr)) {
            remove_selected_region(session, ui);
        }
        ImGui::EndPopup();
    }

    // The drag itself.
    if (dragging_here && ImGui::IsItemActive()) {
        const std::int64_t mouse_frame = x_to_frame(session, view, io.MousePos.x);
        const auto snapped = [&](std::int64_t frame) {
            return io.KeyAlt ? frame : session.snap_frame(frame);
        };
        const bool moved = ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3.0f);
        const bool region_exists = ui.drag_region < track.regions.size();
        switch (ui.drag) {
            case DragKind::Seek:
                if (io.MouseDelta.x != 0.0f) {
                    session.seek_frames(mouse_frame);
                }
                break;
            case DragKind::Move:
                if (moved && region_exists) {
                    session.move_region(track_index, ui.drag_region,
                                        snapped(mouse_frame - ui.drag_grab));
                }
                break;
            case DragKind::TrimStart:
                if (moved && region_exists) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    session.trim_region_start(track_index, ui.drag_region, snapped(mouse_frame));
                }
                break;
            case DragKind::TrimEnd:
                if (moved && region_exists) {
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
                    session.trim_region_end(track_index, ui.drag_region, snapped(mouse_frame));
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
    draw_playhead(session, view, draw_list, origin.y, corner.y);
    draw_list->PopClipRect();
}

// The controls on the left of a lane. Returns true when the track should be removed.
bool draw_track_header(Session& session, std::size_t i, float header_width, float row_height) {
    const TrackInfo& track = session.tracks()[i];
    const float font = ImGui::GetFontSize();
    bool remove = false;

    ImGui::BeginChild("header", ImVec2(header_width, row_height), ImGuiChildFlags_Borders,
                      ImGuiWindowFlags_NoScrollbar);
    if (toggle_button("M", track.mute, ImVec4(0.75f, 0.55f, 0.15f, 1.0f))) {
        session.set_track_mute(i, !track.mute);
    }
    tooltip("Mute");
    ImGui::SameLine();
    if (toggle_button("S", track.solo, ImVec4(0.20f, 0.60f, 0.85f, 1.0f))) {
        session.set_track_solo(i, !track.solo);
    }
    tooltip("Solo");

    // The remove button sits in the far corner, away from mute and solo, and asks first.
    // The track name gets the space in between and is shortened when it does not fit.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float remove_width = ImGui::CalcTextSize("X").x + 2.0f * style.FramePadding.x;
    const float remove_x = ImGui::GetWindowWidth() - remove_width - style.WindowPadding.x;

    ImGui::SameLine();
    const std::string title = std::to_string(i + 1) + ". " + track.name;
    const float title_width = remove_x - ImGui::GetCursorPosX() - style.ItemSpacing.x;
    ImGui::TextUnformatted(fit_text(title, title_width).c_str());
    tooltip(title.c_str());

    ImGui::SameLine(remove_x);
    if (ImGui::SmallButton("X")) {
        ImGui::OpenPopup("remove track");
    }
    tooltip("Remove the track");
    if (ImGui::BeginPopup("remove track")) {
        ImGui::Text("Remove track %d?", static_cast<int>(i + 1));
        if (ImGui::Button("Remove")) {
            remove = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    // The name and the value are written on the sliders, like on the master slider.
    const float slider_width = header_width - 2.0f * font;
    float volume = track.gain * 100.0f;
    ImGui::SetNextItemWidth(slider_width);
    if (ImGui::SliderFloat("##gain", &volume, 0.0f, 200.0f, "Volume: %.0f%%",
                           ImGuiSliderFlags_AlwaysClamp)) {
        session.set_track_gain(i, volume / 100.0f);
    }

    float pan = track.pan * 100.0f;
    char pan_text[48];
    const int pan_amount = static_cast<int>(std::lround(std::fabs(pan)));
    if (pan_amount == 0) {
        std::snprintf(pan_text, sizeof(pan_text), "Pan: centre");
    } else {
        // "%%%%" becomes "%%" here and a single percent sign on the slider.
        std::snprintf(pan_text, sizeof(pan_text), "Pan: %d%%%% %s", pan_amount,
                      pan < 0.0f ? "left" : "right");
    }
    ImGui::SetNextItemWidth(slider_width);
    if (ImGui::SliderFloat("##pan", &pan, -100.0f, 100.0f, pan_text, ImGuiSliderFlags_AlwaysClamp)) {
        session.set_track_pan(i, std::fabs(pan) < 3.0f ? 0.0f : pan / 100.0f);  // snap to centre
    }

    draw_meter(session.track_peak(i), slider_width, 0.75f * font);
    ImGui::EndChild();
    return remove;
}

void draw_tracks(Session& session, UiState& ui) {
    View& view = ui.view;
    const ImGuiIO& io = ImGui::GetIO();
    const float font = ImGui::GetFontSize();
    const float header_width = 17.0f * font;
    const float row_height = 6.2f * font;

    // Mouse wheel over the timeline: Ctrl zooms around the mouse, Shift scrolls sideways.
    if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        if (io.KeyCtrl && io.MouseWheel != 0.0f) {
            zoom_view(view, std::pow(1.25, static_cast<double>(io.MouseWheel)), io.MousePos.x);
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

    std::size_t remove_index = session.tracks().size();
    for (std::size_t i = 0; i < session.tracks().size(); ++i) {
        ImGui::PushID(static_cast<int>(i));
        if (draw_track_header(session, i, header_width, row_height)) {
            remove_index = i;
        }
        ImGui::SameLine();
        draw_lane(session, ui, i, row_height);
        ImGui::PopID();
    }

    if (remove_index < session.tracks().size()) {
        session.remove_track(remove_index);
        ui.selected = false;
        ui.drag = DragKind::None;
    }

    if (session.tracks().empty()) {
        ImGui::Spacing();
        ImGui::TextDisabled("Drop audio files here (WAV, FLAC, MP3) or press \"Add files...\".");
    }
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
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
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

        std::string audio_error;
        if (!session.start_audio(&audio_error)) {
            ui.error = audio_error;
        }
        for (int i = 1; i < argc; ++i) {
            pending.add(argv[i]);  // SDL passes the arguments as UTF-8
        }

        bool running = true;
        while (running) {
            // Sleep while nothing moves on screen; redraw continuously while sound plays.
            const bool animating = session.is_playing() || session.master_peak() > 0.0005f;
            SDL_Event event;
            bool have_event = animating ? SDL_PollEvent(&event) : SDL_WaitEventTimeout(&event, 250);
            while (have_event) {
                ImGui_ImplSDL3_ProcessEvent(&event);
                if (event.type == SDL_EVENT_QUIT) {
                    running = false;
                } else if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                           event.window.windowID == SDL_GetWindowID(window)) {
                    running = false;
                } else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data != nullptr) {
                    pending.add(event.drop.data);
                }
                have_event = SDL_PollEvent(&event);
            }

            for (const std::string& path : pending.take()) {
                std::string error;
                if (!session.add_track_from_file(std::filesystem::u8path(path), &error)) {
                    ui.error = path + ": " + error;
                }
            }
            session.update();

            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();

            if (!io.WantTextInput) {
                if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
                    session.toggle_play();
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
                    session.seek_frames(0);
                    ui.view.start_seconds = 0.0;
                }
                if (ImGui::IsKeyPressed(ImGuiKey_S, false) && !io.KeyCtrl) {
                    split_at_playhead(session, ui);
                }
                if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) {
                    remove_selected_region(session, ui);
                }
            }

            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::Begin("main", nullptr,
                         ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                             ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus);
            draw_toolbar(session, pending, window, ui);
            ImGui::Separator();
            ImGui::BeginChild("tracks");
            draw_tracks(session, ui);
            ImGui::EndChild();
            ImGui::End();

            ImGui::Render();
            SDL_SetRenderDrawColor(renderer, 20, 21, 24, 255);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
        }
    }  // the session (and the audio device) is closed here, before the window goes away

    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
