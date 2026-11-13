// Example CLAP plugins that come with Tone of Hell: Echo, Tremolo and Gain.
//
// They are small on purpose: they show what a CLAP effect needs (a factory, audio ports,
// parameters and a saved state), they give a new user a few effects to try, and the tests use
// them to check the plugin host. Like the rest of the project they are under the MIT license.

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

struct ParamDef {
    clap_id id;
    const char* name;
    double min;
    double max;
    double default_value;
    const char* unit;
};

const ParamDef kEchoParams[] = {{0, "Time", 10.0, 2000.0, 375.0, "ms"},
                                {1, "Feedback", 0.0, 95.0, 40.0, "%"},
                                {2, "Mix", 0.0, 100.0, 35.0, "%"}};
const ParamDef kTremoloParams[] = {{0, "Rate", 0.1, 20.0, 5.0, "Hz"},
                                   {1, "Depth", 0.0, 100.0, 60.0, "%"}};
const ParamDef kGainParams[] = {{0, "Gain", -24.0, 24.0, 0.0, "dB"},
                                {1, "Pan", -100.0, 100.0, 0.0, "%"}};

const char* const kEchoFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_DELAY,
                                     CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const char* const kTremoloFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_TREMOLO,
                                        CLAP_PLUGIN_FEATURE_STEREO, nullptr};
const char* const kGainFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY,
                                     CLAP_PLUGIN_FEATURE_STEREO, nullptr};

const clap_plugin_descriptor_t kEcho = {
    CLAP_VERSION_INIT, "org.toneofhell.echo", "Echo", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "The sound comes back again and again, quieter every time.", kEchoFeatures};
const clap_plugin_descriptor_t kTremolo = {
    CLAP_VERSION_INIT, "org.toneofhell.tremolo", "Tremolo", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "The volume swells and fades in a steady rhythm.", kTremoloFeatures};
const clap_plugin_descriptor_t kGain = {
    CLAP_VERSION_INIT, "org.toneofhell.gain", "Gain", "Tone of Hell",
    "https://github.com/AmmaArkf/Tone-of-Hell", "", "", "1.0.0",
    "Louder or quieter, and to the left or right.", kGainFeatures};

enum class Kind { Echo, Tremolo, Gain };

struct Example {
    clap_plugin_t plugin{};
    Kind kind = Kind::Gain;
    const ParamDef* defs = nullptr;
    std::uint32_t count = 0;
    std::array<std::atomic<double>, 4> values{};
    double rate = 48000.0;
    std::vector<float> delay;  // stereo, interleaved
    std::size_t delay_frames = 0;
    std::size_t write = 0;
    double phase = 0.0;
};

Example& self(const clap_plugin_t* plugin) { return *static_cast<Example*>(plugin->plugin_data); }

void apply_events(Example& e, const clap_input_events_t* events) {
    if (events == nullptr) {
        return;
    }
    const std::uint32_t count = events->size(events);
    for (std::uint32_t i = 0; i < count; ++i) {
        const clap_event_header_t* header = events->get(events, i);
        if (header == nullptr || header->space_id != CLAP_CORE_EVENT_SPACE_ID ||
            header->type != CLAP_EVENT_PARAM_VALUE) {
            continue;
        }
        const auto* event = reinterpret_cast<const clap_event_param_value_t*>(header);
        if (event->param_id < e.count) {
            const ParamDef& def = e.defs[event->param_id];
            e.values[event->param_id].store(std::clamp(event->value, def.min, def.max));
        }
    }
}

// ---- The plugin ----------------------------------------------------------------------------

bool plugin_init(const clap_plugin_t*) { return true; }

void plugin_destroy(const clap_plugin_t* plugin) { delete &self(plugin); }

bool plugin_activate(const clap_plugin_t* plugin, double rate, std::uint32_t, std::uint32_t) {
    Example& e = self(plugin);
    e.rate = rate;
    if (e.kind == Kind::Echo) {
        e.delay_frames = static_cast<std::size_t>(rate * 2.1) + 1;  // a little over 2 seconds
        e.delay.assign(e.delay_frames * 2, 0.0f);
        e.write = 0;
    }
    e.phase = 0.0;
    return true;
}

void plugin_deactivate(const clap_plugin_t*) {}

bool plugin_start(const clap_plugin_t*) { return true; }

void plugin_stop(const clap_plugin_t*) {}

void plugin_reset(const clap_plugin_t* plugin) {
    Example& e = self(plugin);
    std::fill(e.delay.begin(), e.delay.end(), 0.0f);
    e.phase = 0.0;
}

clap_process_status plugin_process(const clap_plugin_t* plugin, const clap_process_t* process) {
    Example& e = self(plugin);
    apply_events(e, process->in_events);
    if (process->audio_inputs_count < 1 || process->audio_outputs_count < 1) {
        return CLAP_PROCESS_ERROR;
    }
    const clap_audio_buffer_t& in = process->audio_inputs[0];
    const clap_audio_buffer_t& out = process->audio_outputs[0];
    if (in.channel_count == 0 || out.channel_count == 0 || in.data32 == nullptr || out.data32 == nullptr) {
        return CLAP_PROCESS_ERROR;
    }
    const auto input = [&in](std::uint32_t channel, std::uint32_t frame) {
        return in.data32[std::min(channel, in.channel_count - 1)][frame];
    };
    const auto output = [&out](std::uint32_t frame, float left, float right) {
        for (std::uint32_t c = 0; c < out.channel_count; ++c) {
            out.data32[c][frame] = c == 0 ? left : (c == 1 ? right : 0.5f * (left + right));
        }
    };

    const std::uint32_t frames = process->frames_count;
    switch (e.kind) {
        case Kind::Echo: {
            const double seconds = e.values[0].load() / 1000.0;
            const auto distance = std::clamp<std::size_t>(static_cast<std::size_t>(seconds * e.rate), 1,
                                                          e.delay_frames - 1);
            const auto feedback = static_cast<float>(e.values[1].load() / 100.0);
            const auto mix = static_cast<float>(e.values[2].load() / 100.0);
            for (std::uint32_t i = 0; i < frames; ++i) {
                const std::size_t read = (e.write + e.delay_frames - distance) % e.delay_frames;
                float result[2];
                for (std::uint32_t c = 0; c < 2; ++c) {
                    const float dry = input(c, i);
                    const float echo = e.delay[read * 2 + c];
                    e.delay[e.write * 2 + c] = dry + echo * feedback;
                    result[c] = dry + echo * mix;
                }
                output(i, result[0], result[1]);
                e.write = (e.write + 1) % e.delay_frames;
            }
            break;
        }
        case Kind::Tremolo: {
            const double step = 2.0 * kPi * e.values[0].load() / e.rate;
            const double depth = e.values[1].load() / 100.0;
            for (std::uint32_t i = 0; i < frames; ++i) {
                const auto gain = static_cast<float>(1.0 - depth * (0.5 - 0.5 * std::cos(e.phase)));
                e.phase = std::fmod(e.phase + step, 2.0 * kPi);
                output(i, input(0, i) * gain, input(1, i) * gain);
            }
            break;
        }
        case Kind::Gain: {
            const double gain = std::pow(10.0, e.values[0].load() / 20.0);
            const double pan = e.values[1].load() / 100.0;
            const auto left = static_cast<float>(gain * std::min(1.0, 1.0 - pan));
            const auto right = static_cast<float>(gain * std::min(1.0, 1.0 + pan));
            for (std::uint32_t i = 0; i < frames; ++i) {
                output(i, input(0, i) * left, input(1, i) * right);
            }
            break;
        }
    }
    return CLAP_PROCESS_CONTINUE;
}

// ---- Audio ports: one stereo input and one stereo output ---------------------------------------

std::uint32_t ports_count(const clap_plugin_t*, bool) { return 1; }

bool ports_get(const clap_plugin_t*, std::uint32_t index, bool is_input, clap_audio_port_info_t* info) {
    if (index != 0) {
        return false;
    }
    *info = clap_audio_port_info_t{};
    info->id = is_input ? 0u : 1u;
    std::snprintf(info->name, sizeof(info->name), "%s", is_input ? "In" : "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}

const clap_plugin_audio_ports_t kPorts = {ports_count, ports_get};

// ---- Parameters ----------------------------------------------------------------------------------

std::uint32_t params_count(const clap_plugin_t* plugin) { return self(plugin).count; }

bool params_info(const clap_plugin_t* plugin, std::uint32_t index, clap_param_info_t* info) {
    const Example& e = self(plugin);
    if (index >= e.count) {
        return false;
    }
    const ParamDef& def = e.defs[index];
    *info = clap_param_info_t{};
    info->id = def.id;
    info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    info->cookie = nullptr;
    std::snprintf(info->name, sizeof(info->name), "%s", def.name);
    info->module[0] = '\0';
    info->min_value = def.min;
    info->max_value = def.max;
    info->default_value = def.default_value;
    return true;
}

bool params_value(const clap_plugin_t* plugin, clap_id id, double* value) {
    const Example& e = self(plugin);
    if (id >= e.count) {
        return false;
    }
    *value = e.values[id].load();
    return true;
}

bool params_to_text(const clap_plugin_t* plugin, clap_id id, double value, char* text, std::uint32_t size) {
    const Example& e = self(plugin);
    if (id >= e.count) {
        return false;
    }
    std::snprintf(text, size, value >= 100.0 || value <= -100.0 ? "%.0f %s" : "%.1f %s", value, e.defs[id].unit);
    return true;
}

bool params_from_text(const clap_plugin_t* plugin, clap_id id, const char* text, double* value) {
    if (id >= self(plugin).count) {
        return false;
    }
    *value = std::strtod(text, nullptr);
    return true;
}

void params_flush(const clap_plugin_t* plugin, const clap_input_events_t* in, const clap_output_events_t*) {
    apply_events(self(plugin), in);
}

const clap_plugin_params_t kParams = {params_count, params_info, params_value,
                                      params_to_text, params_from_text, params_flush};

// ---- State: the parameter values -----------------------------------------------------------------

bool state_save(const clap_plugin_t* plugin, const clap_ostream_t* stream) {
    const Example& e = self(plugin);
    std::array<double, 4> data{};
    for (std::uint32_t i = 0; i < e.count; ++i) {
        data[i] = e.values[i].load();
    }
    const auto* bytes = reinterpret_cast<const char*>(data.data());
    std::uint64_t left = sizeof(double) * e.count;
    while (left > 0) {
        const std::int64_t written = stream->write(stream, bytes, left);
        if (written <= 0) {
            return false;
        }
        bytes += written;
        left -= static_cast<std::uint64_t>(written);
    }
    return true;
}

bool state_load(const clap_plugin_t* plugin, const clap_istream_t* stream) {
    Example& e = self(plugin);
    std::array<double, 4> data{};
    auto* bytes = reinterpret_cast<char*>(data.data());
    std::uint64_t left = sizeof(double) * e.count;
    while (left > 0) {
        const std::int64_t read = stream->read(stream, bytes, left);
        if (read <= 0) {
            return false;
        }
        bytes += read;
        left -= static_cast<std::uint64_t>(read);
    }
    for (std::uint32_t i = 0; i < e.count; ++i) {
        e.values[i].store(std::clamp(data[i], e.defs[i].min, e.defs[i].max));
    }
    return true;
}

const clap_plugin_state_t kState = {state_save, state_load};

const void* plugin_extension(const clap_plugin_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0) {
        return &kPorts;
    }
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
        return &kParams;
    }
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {
        return &kState;
    }
    return nullptr;
}

void plugin_on_main_thread(const clap_plugin_t*) {}

// ---- The factory -----------------------------------------------------------------------------------

const clap_plugin_descriptor_t* const kDescriptors[] = {&kEcho, &kTremolo, &kGain};
constexpr std::uint32_t kPluginCount = 3;

std::uint32_t factory_count(const clap_plugin_factory_t*) { return kPluginCount; }

const clap_plugin_descriptor_t* factory_descriptor(const clap_plugin_factory_t*, std::uint32_t index) {
    return index < kPluginCount ? kDescriptors[index] : nullptr;
}

const clap_plugin_t* factory_create(const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
    if (host == nullptr || id == nullptr || !clap_version_is_compatible(host->clap_version)) {
        return nullptr;
    }
    auto* e = new Example();
    if (std::strcmp(id, kEcho.id) == 0) {
        e->kind = Kind::Echo;
        e->defs = kEchoParams;
        e->count = 3;
        e->plugin.desc = &kEcho;
    } else if (std::strcmp(id, kTremolo.id) == 0) {
        e->kind = Kind::Tremolo;
        e->defs = kTremoloParams;
        e->count = 2;
        e->plugin.desc = &kTremolo;
    } else if (std::strcmp(id, kGain.id) == 0) {
        e->kind = Kind::Gain;
        e->defs = kGainParams;
        e->count = 2;
        e->plugin.desc = &kGain;
    } else {
        delete e;
        return nullptr;
    }
    for (std::uint32_t i = 0; i < e->count; ++i) {
        e->values[i].store(e->defs[i].default_value);
    }
    e->plugin.plugin_data = e;
    e->plugin.init = plugin_init;
    e->plugin.destroy = plugin_destroy;
    e->plugin.activate = plugin_activate;
    e->plugin.deactivate = plugin_deactivate;
    e->plugin.start_processing = plugin_start;
    e->plugin.stop_processing = plugin_stop;
    e->plugin.reset = plugin_reset;
    e->plugin.process = plugin_process;
    e->plugin.get_extension = plugin_extension;
    e->plugin.on_main_thread = plugin_on_main_thread;
    return &e->plugin;
}

const clap_plugin_factory_t kFactory = {factory_count, factory_descriptor, factory_create};

bool entry_init(const char*) { return true; }

void entry_deinit() {}

const void* entry_factory(const char* id) {
    return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &kFactory : nullptr;
}

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, entry_init, entry_deinit,
                                                                entry_factory};
