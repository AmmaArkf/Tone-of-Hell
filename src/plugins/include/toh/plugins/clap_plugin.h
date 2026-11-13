#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "toh/core/effect.h"

namespace toh::plugins {

/// What a plugin file says about one of its plugins.
struct PluginInfo {
    std::filesystem::path path;  ///< the .clap file
    std::string id;              ///< for example "org.toneofhell.echo"
    std::string name;
    std::string vendor;
    std::string version;
    std::string description;
    bool instrument = false;     ///< it makes sound from notes
};

/// The usual folders for CLAP plugins on this system (CLAP_PATH first).
std::vector<std::filesystem::path> default_clap_folders();

/// The plugins of one .clap file. Empty when it cannot be loaded; `error` says why.
std::vector<PluginInfo> read_clap_file(const std::filesystem::path& file, std::string* error = nullptr);

/// Every plugin in the .clap files of `folders` and their subfolders, sorted by name. Files
/// that cannot be loaded are skipped and described in `problems`.
std::vector<PluginInfo> scan_clap_folders(const std::vector<std::filesystem::path>& folders,
                                          std::vector<std::string>* problems = nullptr);

/// One CLAP plugin, used as an insert effect of a track. process() runs on the audio thread;
/// everything else must be called from the thread that created it (the main thread).
class ClapPlugin final : public core::AudioEffect {
public:
    struct Param {
        std::uint32_t id = 0;
        std::string name;
        std::string module;  ///< a group like "Filter/Envelope", may be empty
        double min = 0.0;
        double max = 1.0;
        double default_value = 0.0;
        bool stepped = false;  ///< whole numbers only
        bool hidden = false;   ///< hidden or read-only: not for the user to change
    };

    /// Loads the plugin `id` of a .clap file and makes it ready to process sound at
    /// `sample_rate` with `channels` channels. Returns null when that fails; `error` says why.
    static std::shared_ptr<ClapPlugin> create(const std::filesystem::path& file, const std::string& id,
                                              double sample_rate, std::uint32_t channels,
                                              std::string* error = nullptr);
    ~ClapPlugin() override;

    const PluginInfo& info() const noexcept;

    void process(float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept override;

    // ---- Parameters ----------------------------------------------------------------------
    const std::vector<Param>& params() const noexcept;
    double param_value(std::uint32_t id) const;
    /// The value as the plugin writes it, for example "375 ms".
    std::string param_text(std::uint32_t id, double value) const;
    /// Sends a new value; the plugin gets it with the next block it processes.
    bool set_param(std::uint32_t id, double value);

    // ---- State ---------------------------------------------------------------------------
    /// Everything the plugin needs to come back as it is: saved inside the project.
    bool save_state(std::vector<char>& state) const;
    bool load_state(const std::vector<char>& state);

    // ---- The plugin's own window -----------------------------------------------------------
    bool has_gui() const;
    bool gui_open() const noexcept;
    /// Puts the plugin's window inside `parent`: a HWND on Windows, an X11 window id on Linux.
    /// `width` and `height` receive its size, `resizable` whether it can change.
    bool open_gui(void* parent, std::uint32_t& width, std::uint32_t& height, bool& resizable);
    /// Asks for another size; `width` and `height` receive the size it really takes.
    bool resize_gui(std::uint32_t& width, std::uint32_t& height);
    void close_gui();
    /// A size the plugin asked its window to have, once.
    bool take_resize_request(std::uint32_t& width, std::uint32_t& height);
    /// True once when the plugin closed its window by itself.
    bool take_gui_closed();

    /// Main thread, often (every frame): answers what the plugin asked for.
    void idle();

    struct Impl;

private:
    ClapPlugin();
    std::unique_ptr<Impl> impl_;
};

}  // namespace toh::plugins
