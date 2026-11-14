#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "toh/plugins/plugin.h"

namespace toh::plugins {

/// The usual folders for VST3 plugins on this system.
std::vector<std::filesystem::path> default_vst3_folders();

/// The audio plugins of one .vst3 bundle (or single .vst3 file). Empty when it cannot be
/// loaded; `error` says why.
std::vector<PluginInfo> read_vst3_file(const std::filesystem::path& bundle, std::string* error = nullptr);

/// Every plugin in the .vst3 bundles of `folders` and their subfolders, sorted by name.
std::vector<PluginInfo> scan_vst3_folders(const std::vector<std::filesystem::path>& folders,
                                          std::vector<std::string>* problems = nullptr);

/// One VST3 plugin (a component with its edit controller).
class Vst3Plugin final : public Plugin {
public:
    static std::shared_ptr<Vst3Plugin> create(const std::filesystem::path& bundle, const std::string& id,
                                              double sample_rate, std::uint32_t channels,
                                              std::string* error = nullptr);
    ~Vst3Plugin() override;

    const PluginInfo& info() const noexcept override;
    void process(float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept override;
    void add_note(std::uint32_t frame, bool on, int pitch, float velocity) noexcept override;
    void reset() noexcept override;
    void set_param_rt(std::uint32_t id, double value) noexcept override;

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
    Vst3Plugin();
    std::unique_ptr<Impl> impl_;
};

}  // namespace toh::plugins
