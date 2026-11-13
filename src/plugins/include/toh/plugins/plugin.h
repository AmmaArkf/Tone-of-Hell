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
    std::string format = "clap";  ///< "clap" or "vst3"
    std::filesystem::path path;   ///< the .clap file or the .vst3 bundle
    std::string id;               ///< CLAP: for example "org.toneofhell.echo"; VST3: the class id in hex
    std::string name;
    std::string vendor;
    std::string version;
    std::string description;
    bool instrument = false;      ///< it makes sound from notes
};

/// A plugin used on a track, whatever its format. process() runs on the audio thread;
/// everything else must be called from the thread that created it (the main thread).
class Plugin : public core::AudioEffect {
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

    virtual const PluginInfo& info() const noexcept = 0;

    // ---- Parameters ----------------------------------------------------------------------
    virtual const std::vector<Param>& params() const noexcept = 0;
    virtual double param_value(std::uint32_t id) const = 0;
    /// The value as the plugin writes it, for example "375 ms".
    virtual std::string param_text(std::uint32_t id, double value) const = 0;
    /// Sends a new value; the plugin gets it with the next block it processes.
    virtual bool set_param(std::uint32_t id, double value) = 0;

    // ---- State ---------------------------------------------------------------------------
    /// Everything the plugin needs to come back as it is: saved inside the project.
    virtual bool save_state(std::vector<char>& state) const = 0;
    virtual bool load_state(const std::vector<char>& state) = 0;

    // ---- The plugin's own window -----------------------------------------------------------
    virtual bool has_gui() const = 0;
    virtual bool gui_open() const noexcept = 0;
    /// Puts the plugin's window inside `parent`: a HWND on Windows, an X11 window id on Linux.
    /// `width` and `height` receive its size, `resizable` whether it can change.
    virtual bool open_gui(void* parent, std::uint32_t& width, std::uint32_t& height, bool& resizable) = 0;
    /// Asks for another size; `width` and `height` receive the size it really takes.
    virtual bool resize_gui(std::uint32_t& width, std::uint32_t& height) = 0;
    virtual void close_gui() = 0;
    /// A size the plugin asked its window to have, once.
    virtual bool take_resize_request(std::uint32_t& width, std::uint32_t& height) = 0;
    /// True once when the plugin closed its window by itself.
    virtual bool take_gui_closed() = 0;

    /// Main thread, often (every frame): answers what the plugin asked for.
    virtual void idle() = 0;

    /// How many frames the plugin delays the sound it processes.
    virtual std::uint32_t latency() const noexcept = 0;
    /// True once after the plugin said that its latency changed.
    virtual bool take_latency_changed() = 0;
    /// True for a plugin that makes its sound from notes.
    virtual bool is_instrument() const noexcept = 0;
};

/// Loads a plugin of any format and makes it ready to process sound at `sample_rate` with
/// `channels` channels. Returns null when that fails; `error` says why.
std::shared_ptr<Plugin> create_plugin(const std::string& format, const std::filesystem::path& file,
                                      const std::string& id, double sample_rate, std::uint32_t channels,
                                      std::string* error = nullptr);

/// The usual folders for plugins of both formats, and every plugin found in them (and in
/// `extra` folders first), sorted by name.
std::vector<PluginInfo> scan_all_plugins(const std::vector<std::filesystem::path>& extra,
                                         std::vector<std::string>* problems = nullptr);

}  // namespace toh::plugins
