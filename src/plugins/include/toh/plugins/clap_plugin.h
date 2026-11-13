#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "toh/plugins/plugin.h"

namespace toh::plugins {

/// The usual folders for CLAP plugins on this system (CLAP_PATH first).
std::vector<std::filesystem::path> default_clap_folders();

/// The plugins of one .clap file. Empty when it cannot be loaded; `error` says why.
std::vector<PluginInfo> read_clap_file(const std::filesystem::path& file, std::string* error = nullptr);

/// Every plugin in the .clap files of `folders` and their subfolders, sorted by name. Files
/// that cannot be loaded are skipped and described in `problems`.
std::vector<PluginInfo> scan_clap_folders(const std::vector<std::filesystem::path>& folders,
                                          std::vector<std::string>* problems = nullptr);

/// One CLAP plugin.
class ClapPlugin final : public Plugin {
public:
    /// Loads the plugin `id` of a .clap file and makes it ready to process sound at
    /// `sample_rate` with `channels` channels. Returns null when that fails; `error` says why.
    static std::shared_ptr<ClapPlugin> create(const std::filesystem::path& file, const std::string& id,
                                              double sample_rate, std::uint32_t channels,
                                              std::string* error = nullptr);
    ~ClapPlugin() override;

    const PluginInfo& info() const noexcept override;
    void process(float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept override;
    void add_note(std::uint32_t frame, bool on, int pitch, float velocity) noexcept override;
    void reset() noexcept override;

    const std::vector<Param>& params() const noexcept override;
    double param_value(std::uint32_t id) const override;
    std::string param_text(std::uint32_t id, double value) const override;
    bool set_param(std::uint32_t id, double value) override;

    bool save_state(std::vector<char>& state) const override;
    bool load_state(const std::vector<char>& state) override;

    bool has_gui() const override;
    bool gui_open() const noexcept override;
    bool open_gui(void* parent, std::uint32_t& width, std::uint32_t& height, bool& resizable) override;
    bool resize_gui(std::uint32_t& width, std::uint32_t& height) override;
    void close_gui() override;
    bool take_resize_request(std::uint32_t& width, std::uint32_t& height) override;
    bool take_gui_closed() override;

    void idle() override;
    std::uint32_t latency() const noexcept override;
    bool take_latency_changed() override;
    bool is_instrument() const noexcept override;

    struct Impl;

private:
    ClapPlugin();
    std::unique_ptr<Impl> impl_;
};

}  // namespace toh::plugins
