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

using toh::app::Session;
using toh::app::TrackInfo;

// Files waiting to be loaded. The file dialog may answer on another thread.
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

std::string format_time(std::int64_t frames, std::uint32_t sample_rate) {
    const double seconds = static_cast<double>(frames) / static_cast<double>(sample_rate);
    const int minutes = static_cast<int>(seconds / 60.0);
    char text[32];
    std::snprintf(text, sizeof(text), "%d:%06.3f", minutes, seconds - minutes * 60.0);
    return text;
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

struct UiState {
    std::string error;
    std::uint64_t seen_clips = 0;  // clip count when the indicator was last reset
    std::vector<double> taps;      // times of the latest "Tap tempo" presses, in seconds
};

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
    tooltip("Tempo. Drag left or right, or double-click to type a value.");

    ImGui::SameLine();
    if (ImGui::Button("...##metronome")) {
        ImGui::OpenPopup("metronome settings");
    }
    tooltip("Metronome settings");

    if (ImGui::BeginPopup("metronome settings")) {
        ImGui::TextUnformatted("Metronome");
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
        if (ImGui::SliderFloat("Volume", &settings.volume, 0.0f, 2.0f, "%.2f",
                               ImGuiSliderFlags_AlwaysClamp)) {
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
    if (ImGui::Button(session.is_playing() ? "Pause" : "Play")) {
        session.toggle_play();
    }
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
    if (ImGui::Button("Add files...")) {
        static const SDL_DialogFileFilter filters[] = {{"Audio files", "wav;flac;mp3"},
                                                       {"All files", "*"}};
        SDL_ShowOpenFileDialog(&file_dialog_callback, &pending, window, filters, 2, nullptr, true);
    }

    ImGui::SameLine();
    ImGui::Text("%s / %s", format_time(session.position_frames(), session.sample_rate()).c_str(),
                format_time(session.length_frames(), session.sample_rate()).c_str());

    draw_metronome(session, ui);

    ImGui::SameLine();
    float gain = session.master_gain();
    ImGui::SetNextItemWidth(10.0f * ImGui::GetFontSize());
    if (ImGui::SliderFloat("Master", &gain, 0.0f, 2.0f, "%.2f")) {
        session.set_master_gain(gain);
    }
    ImGui::SameLine();
    draw_meter(session.master_peak(), 8.0f * ImGui::GetFontSize(), ImGui::GetFrameHeight());

    const std::uint64_t clips = session.master_clip_count();
    if (clips > ui.seen_clips) {
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.15f, 0.15f, 1.0f));
        if (ImGui::SmallButton("CLIP")) {
            ui.seen_clips = clips;  // click to reset the indicator
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("The mix went above full scale. Lower the faders. Click to reset.");
        }
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

// Handles clicks and drags on a timeline area: moves the playhead to the mouse position.
void seek_on_click(Session& session, const ImVec2& origin, float width) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool clicked = ImGui::IsItemActivated();
    const bool dragged = ImGui::IsItemActive() && io.MouseDelta.x != 0.0f;
    if ((clicked || dragged) && width > 0.0f && session.length_frames() > 0) {
        const float fraction = std::clamp((io.MousePos.x - origin.x) / width, 0.0f, 1.0f);
        session.seek_frames(static_cast<std::int64_t>(
            static_cast<double>(fraction) * static_cast<double>(session.length_frames())));
    }
}

void draw_playhead(Session& session, ImDrawList* draw_list, const ImVec2& origin, float width,
                   float height) {
    if (session.length_frames() <= 0) {
        return;
    }
    const float x = origin.x + width * static_cast<float>(static_cast<double>(session.position_frames()) /
                                                           static_cast<double>(session.length_frames()));
    draw_list->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + height), IM_COL32(255, 255, 255, 230),
                       1.5f);
}

void draw_ruler(Session& session, float header_width) {
    const float height = 1.5f * ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(header_width, height));
    ImGui::SameLine();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
    ImGui::InvisibleButton("ruler", ImVec2(width, height));
    seek_on_click(session, origin, width);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                             IM_COL32(40, 42, 48, 255));

    const double seconds =
        static_cast<double>(session.length_frames()) / static_cast<double>(session.sample_rate());
    if (seconds > 0.0) {
        // Pick a tick distance that keeps labels at least about 80 pixels apart.
        static const double steps[] = {0.1, 0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300, 600};
        double step = steps[0];
        for (const double candidate : steps) {
            step = candidate;
            if (candidate / seconds * static_cast<double>(width) >= 5.0 * ImGui::GetFontSize()) {
                break;
            }
        }
        for (double t = 0.0; t <= seconds; t += step) {
            const float x = origin.x + static_cast<float>(t / seconds) * width;
            draw_list->AddLine(ImVec2(x, origin.y + height * 0.5f), ImVec2(x, origin.y + height),
                               IM_COL32(150, 150, 160, 255));
            char label[32];
            const int minutes = static_cast<int>(t / 60.0);
            if (step < 1.0) {
                std::snprintf(label, sizeof(label), "%d:%04.1f", minutes, t - minutes * 60.0);
            } else {
                std::snprintf(label, sizeof(label), "%d:%02d", minutes,
                              static_cast<int>(t - minutes * 60.0));
            }
            draw_list->AddText(ImVec2(x + 3.0f, origin.y), IM_COL32(190, 190, 200, 255), label);
        }
    }
    draw_playhead(session, draw_list, origin, width, height);
}

void draw_waveform(Session& session, const TrackInfo& track, const ImVec2& origin, float width,
                   float height) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                             IM_COL32(28, 30, 34, 255));

    const std::int64_t length = session.length_frames();
    const std::int64_t clip_frames = track.clip->frames();
    if (length > 0 && clip_frames > 0) {
        const float clip_width = width * static_cast<float>(static_cast<double>(clip_frames) /
                                                            static_cast<double>(length));
        draw_list->AddRectFilled(origin, ImVec2(origin.x + clip_width, origin.y + height),
                                 IM_COL32(44, 62, 84, 255), 3.0f);

        const ImU32 colour = track.mute ? IM_COL32(110, 120, 130, 255) : IM_COL32(120, 190, 255, 255);
        const float middle = origin.y + height * 0.5f;
        const float half = height * 0.5f - 2.0f;
        const double frames_per_pixel = static_cast<double>(length) / static_cast<double>(width);
        const int columns = static_cast<int>(clip_width);
        for (int x = 0; x < columns; ++x) {
            const auto first = static_cast<std::int64_t>(x * frames_per_pixel);
            const auto last = std::max(first, static_cast<std::int64_t>((x + 1) * frames_per_pixel) - 1);
            float low = 0.0f;
            float high = 0.0f;
            if (!track.overview.range(first, last, low, high)) {
                continue;
            }
            low = std::clamp(low, -1.0f, 1.0f);
            high = std::clamp(high, -1.0f, 1.0f);
            const float px = origin.x + static_cast<float>(x) + 0.5f;
            draw_list->AddLine(ImVec2(px, middle - high * half), ImVec2(px, middle - low * half + 1.0f),
                               colour);
        }
    }
    draw_playhead(session, draw_list, origin, width, height);
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

void draw_tracks(Session& session) {
    const float font = ImGui::GetFontSize();
    const float header_width = 17.0f * font;
    const float row_height = 6.2f * font;

    draw_ruler(session, header_width);

    std::size_t remove_index = session.tracks().size();
    for (std::size_t i = 0; i < session.tracks().size(); ++i) {
        const TrackInfo& track = session.tracks()[i];
        ImGui::PushID(static_cast<int>(i));

        ImGui::BeginChild("header", ImVec2(header_width, row_height), ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar);
        if (toggle_button("M", track.mute, ImVec4(0.75f, 0.55f, 0.15f, 1.0f))) {
            session.set_track_mute(i, !track.mute);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Mute");
        }
        ImGui::SameLine();
        if (toggle_button("S", track.solo, ImVec4(0.20f, 0.60f, 0.85f, 1.0f))) {
            session.set_track_solo(i, !track.solo);
        }
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Solo");
        }
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
                remove_index = i;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        const float slider_width = header_width - 2.0f * font;
        float gain = track.gain;
        ImGui::SetNextItemWidth(slider_width);
        if (ImGui::SliderFloat("##gain", &gain, 0.0f, 2.0f, "volume %.2f")) {
            session.set_track_gain(i, gain);
        }
        float pan = track.pan;
        ImGui::SetNextItemWidth(slider_width);
        if (ImGui::SliderFloat("##pan", &pan, -1.0f, 1.0f, "pan %.2f")) {
            session.set_track_pan(i, std::fabs(pan) < 0.03f ? 0.0f : pan);  // snap to the centre
        }
        draw_meter(session.track_peak(i), slider_width, 0.75f * font);
        ImGui::EndChild();

        ImGui::SameLine();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
        ImGui::InvisibleButton("lane", ImVec2(width, row_height));
        seek_on_click(session, origin, width);
        draw_waveform(session, track, origin, width, row_height);

        ImGui::PopID();
    }

    if (remove_index < session.tracks().size()) {
        session.remove_track(remove_index);
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
            draw_tracks(session);
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
