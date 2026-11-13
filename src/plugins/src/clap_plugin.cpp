#include "toh/plugins/clap_plugin.h"

#include <clap/clap.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <system_error>
#include <thread>
#include <utility>

#include "toh/core/spsc_queue.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace toh::plugins {

namespace {

constexpr std::uint32_t kMaxFrames = 4096;  // the longest block a plugin is given at once
constexpr std::uint32_t kMaxChannels = 8;

#ifdef _WIN32
const char* const kWindowApi = CLAP_WINDOW_API_WIN32;
#else
const char* const kWindowApi = CLAP_WINDOW_API_X11;
#endif

thread_local bool t_audio_thread = false;  // set on the thread that calls process()

void set_error(std::string* error, const std::string& text) {
    if (error != nullptr) {
        *error = text;
    }
}

// A loaded .clap file. Its entry is initialized while it is loaded, and one file is loaded only
// once however many plugins of it are in use.
class Library {
public:
    static std::shared_ptr<Library> open(const std::filesystem::path& file, std::string* error);
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    const clap_plugin_factory_t* factory() const noexcept {
        return static_cast<const clap_plugin_factory_t*>(entry_->get_factory(CLAP_PLUGIN_FACTORY_ID));
    }

private:
    Library() = default;
#ifdef _WIN32
    HMODULE module_ = nullptr;
#else
    void* module_ = nullptr;
#endif
    const clap_plugin_entry_t* entry_ = nullptr;
    bool initialized_ = false;
};

std::map<std::filesystem::path, std::weak_ptr<Library>>& loaded_libraries() {
    static std::map<std::filesystem::path, std::weak_ptr<Library>> libraries;
    return libraries;
}

std::shared_ptr<Library> Library::open(const std::filesystem::path& file, std::string* error) {
    std::error_code code;
    std::filesystem::path key = std::filesystem::weakly_canonical(file, code);
    if (code) {
        key = file;
    }
    auto& libraries = loaded_libraries();
    const auto found = libraries.find(key);
    if (found != libraries.end()) {
        if (auto alive = found->second.lock()) {
            return alive;
        }
    }

    std::shared_ptr<Library> library(new Library());
#ifdef _WIN32
    library->module_ = LoadLibraryW(file.wstring().c_str());
    if (library->module_ == nullptr) {
        set_error(error, "cannot load " + file.u8string());
        return nullptr;
    }
    const FARPROC symbol = GetProcAddress(library->module_, "clap_entry");
    static_assert(sizeof(symbol) == sizeof(library->entry_), "a symbol is a pointer");
    if (symbol != nullptr) {
        std::memcpy(&library->entry_, &symbol, sizeof(library->entry_));
    }
#else
    library->module_ = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (library->module_ == nullptr) {
        const char* reason = dlerror();
        set_error(error, "cannot load " + file.u8string() + (reason != nullptr ? std::string(": ") + reason : ""));
        return nullptr;
    }
    library->entry_ = static_cast<const clap_plugin_entry_t*>(dlsym(library->module_, "clap_entry"));
#endif
    if (library->entry_ == nullptr) {
        set_error(error, file.u8string() + " is not a CLAP plugin (it has no clap_entry)");
        return nullptr;
    }
    if (!clap_version_is_compatible(library->entry_->clap_version)) {
        set_error(error, file.u8string() + " needs another version of CLAP");
        return nullptr;
    }
    const std::string path_text = file.u8string();
    if (!library->entry_->init(path_text.c_str())) {
        set_error(error, file.u8string() + " did not start");
        return nullptr;
    }
    library->initialized_ = true;
    if (library->factory() == nullptr) {
        set_error(error, file.u8string() + " has no plugin factory");
        return nullptr;
    }
    libraries[key] = library;
    return library;
}

Library::~Library() {
    if (initialized_) {
        entry_->deinit();
    }
#ifdef _WIN32
    if (module_ != nullptr) {
        FreeLibrary(module_);
    }
#else
    if (module_ != nullptr) {
        dlclose(module_);
    }
#endif
}

PluginInfo describe(const clap_plugin_descriptor_t& descriptor, const std::filesystem::path& file) {
    const auto text = [](const char* value) { return value != nullptr ? std::string(value) : std::string(); };
    PluginInfo info;
    info.path = file;
    info.id = text(descriptor.id);
    info.name = text(descriptor.name);
    info.vendor = text(descriptor.vendor);
    info.version = text(descriptor.version);
    info.description = text(descriptor.description);
    if (info.name.empty()) {
        info.name = info.id;
    }
    if (descriptor.features != nullptr) {
        for (const char* const* feature = descriptor.features; *feature != nullptr; ++feature) {
            if (std::strcmp(*feature, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0) {
                info.instrument = true;
            }
        }
    }
    return info;
}

struct ParamChange {
    std::uint32_t id = 0;
    double value = 0.0;
};

struct PendingNote {
    std::uint32_t frame = 0;
    bool on = false;
    std::int16_t key = -1;
    float velocity = 0.0f;
};

#ifdef _WIN32
std::filesystem::path environment_path(const wchar_t* name) {
    std::vector<wchar_t> buffer(32768);
    const DWORD length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) {
        return {};
    }
    return std::filesystem::path(std::wstring(buffer.data(), length));
}
#endif

}  // namespace

struct ClapPlugin::Impl {
    std::shared_ptr<Library> library;
    PluginInfo info;
    clap_host_t host{};
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_params_t* params = nullptr;
    const clap_plugin_state_t* state = nullptr;
    const clap_plugin_gui_t* gui = nullptr;
    std::vector<Param> param_list;
    std::uint32_t in_channels = 2;   // 0: no audio input (an instrument)
    std::uint32_t out_channels = 2;
    bool active = false;
    std::atomic<bool> processing{false};  // started on the audio thread
    std::atomic<bool> ending{false};      // being destroyed: the main thread stands in for it
    std::thread::id main_thread;

    std::atomic<bool> callback_requested{false};
    std::atomic<bool> rescan_requested{false};
    std::atomic<bool> gui_closed{false};
    std::atomic<bool> resize_requested{false};
    std::atomic<std::uint32_t> resize_width{0};
    std::atomic<std::uint32_t> resize_height{0};
    bool gui_created = false;

    std::vector<float> in_buffer;
    std::vector<float> out_buffer;
    std::array<float*, kMaxChannels> in_ptrs{};
    std::array<float*, kMaxChannels> out_ptrs{};

    core::SpscQueue<ParamChange, 1024> changes;
    std::uint64_t sent = 0;                  // main thread: changes sent so far
    std::atomic<std::uint64_t> consumed{0};  // audio thread: changes given to the plugin so far
    std::map<std::uint32_t, std::pair<double, std::uint64_t>> requested;  // value, change number
    std::array<clap_event_param_value_t, 256> events{};
    std::array<PendingNote, 512> pending{};  // notes for the next block (audio thread)
    std::uint32_t pending_count = 0;
    std::array<clap_event_note_t, 512> note_events{};
    std::array<const clap_event_header_t*, 768> order{};  // every event of the block, by time
    std::uint32_t event_count = 0;
    std::int64_t steady_time = 0;
};

namespace {

ClapPlugin::Impl& impl_of(const clap_host_t* host) {
    return *static_cast<ClapPlugin::Impl*>(host->host_data);
}

const clap_host_thread_check_t kThreadCheck = {
    [](const clap_host_t* host) -> bool {
        return std::this_thread::get_id() == impl_of(host).main_thread && !t_audio_thread;
    },
    [](const clap_host_t* host) -> bool { return t_audio_thread || impl_of(host).ending.load(); },
};

const clap_host_log_t kLog = {
    [](const clap_host_t*, clap_log_severity, const char*) {},
};

const clap_host_params_t kHostParams = {
    [](const clap_host_t* host, clap_param_rescan_flags) { impl_of(host).rescan_requested = true; },
    [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
    [](const clap_host_t*) {},
};

const clap_host_state_t kHostState = {
    [](const clap_host_t*) {},
};

const clap_host_gui_t kHostGui = {
    [](const clap_host_t*) {},
    [](const clap_host_t* host, std::uint32_t width, std::uint32_t height) -> bool {
        ClapPlugin::Impl& s = impl_of(host);
        s.resize_width = width;
        s.resize_height = height;
        s.resize_requested = true;
        return true;
    },
    [](const clap_host_t*) -> bool { return true; },
    [](const clap_host_t*) -> bool { return true; },
    [](const clap_host_t* host, bool) { impl_of(host).gui_closed = true; },
};

const void* host_extension(const clap_host_t*, const char* id) {
    if (std::strcmp(id, CLAP_EXT_THREAD_CHECK) == 0) {
        return &kThreadCheck;
    }
    if (std::strcmp(id, CLAP_EXT_LOG) == 0) {
        return &kLog;
    }
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) {
        return &kHostParams;
    }
    if (std::strcmp(id, CLAP_EXT_STATE) == 0) {
        return &kHostState;
    }
    if (std::strcmp(id, CLAP_EXT_GUI) == 0) {
        return &kHostGui;
    }
    return nullptr;
}

std::uint32_t events_size(const clap_input_events_t* list) {
    return static_cast<const ClapPlugin::Impl*>(list->ctx)->event_count;
}

const clap_event_header_t* events_get(const clap_input_events_t* list, std::uint32_t index) {
    const auto* s = static_cast<const ClapPlugin::Impl*>(list->ctx);
    return index < s->event_count ? s->order[index] : nullptr;
}

bool events_push(const clap_output_events_t*, const clap_event_header_t*) { return true; }

void load_params(ClapPlugin::Impl& s) {
    s.param_list.clear();
    if (s.params == nullptr) {
        return;
    }
    const std::uint32_t count = s.params->count(s.plugin);
    for (std::uint32_t i = 0; i < count; ++i) {
        clap_param_info_t info{};
        if (!s.params->get_info(s.plugin, i, &info)) {
            continue;
        }
        ClapPlugin::Param param;
        param.id = info.id;
        param.name = info.name;
        param.module = info.module;
        param.min = info.min_value;
        param.max = std::max(info.max_value, info.min_value);
        param.default_value = std::clamp(info.default_value, param.min, param.max);
        param.stepped = (info.flags & CLAP_PARAM_IS_STEPPED) != 0;
        param.hidden = (info.flags & (CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY)) != 0;
        s.param_list.push_back(std::move(param));
    }
}

}  // namespace

// ---- Finding plugins ---------------------------------------------------------------------

std::vector<std::filesystem::path> default_clap_folders() {
    std::vector<std::filesystem::path> folders;
#ifdef _WIN32
    const std::wstring list = environment_path(L"CLAP_PATH").wstring();
    std::size_t begin = 0;
    while (begin < list.size()) {
        std::size_t end = list.find(L';', begin);
        if (end == std::wstring::npos) {
            end = list.size();
        }
        if (end > begin) {
            folders.emplace_back(list.substr(begin, end - begin));
        }
        begin = end + 1;
    }
    const std::filesystem::path common = environment_path(L"COMMONPROGRAMFILES");
    if (!common.empty()) {
        folders.push_back(common / "CLAP");
    }
    const std::filesystem::path local = environment_path(L"LOCALAPPDATA");
    if (!local.empty()) {
        folders.push_back(local / "Programs" / "Common" / "CLAP");
    }
#else
    if (const char* list = std::getenv("CLAP_PATH")) {
        const std::string text = list;
        std::size_t begin = 0;
        while (begin < text.size()) {
            std::size_t end = text.find(':', begin);
            if (end == std::string::npos) {
                end = text.size();
            }
            if (end > begin) {
                folders.emplace_back(text.substr(begin, end - begin));
            }
            begin = end + 1;
        }
    }
    if (const char* home = std::getenv("HOME")) {
        folders.push_back(std::filesystem::path(home) / ".clap");
    }
    folders.emplace_back("/usr/lib/clap");
    folders.emplace_back("/usr/local/lib/clap");
#endif
    return folders;
}

std::vector<PluginInfo> read_clap_file(const std::filesystem::path& file, std::string* error) {
    const std::shared_ptr<Library> library = Library::open(file, error);
    if (!library) {
        return {};
    }
    const clap_plugin_factory_t* factory = library->factory();
    std::vector<PluginInfo> infos;
    const std::uint32_t count = factory->get_plugin_count(factory);
    for (std::uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* descriptor = factory->get_plugin_descriptor(factory, i);
        if (descriptor != nullptr && descriptor->id != nullptr) {
            infos.push_back(describe(*descriptor, file));
        }
    }
    if (infos.empty()) {
        set_error(error, file.u8string() + " contains no plugins");
    }
    return infos;
}

std::vector<PluginInfo> scan_clap_folders(const std::vector<std::filesystem::path>& folders,
                                          std::vector<std::string>* problems) {
    std::vector<PluginInfo> result;
    std::set<std::filesystem::path> seen;
    for (const std::filesystem::path& folder : folders) {
        std::error_code code;
        if (folder.empty() || !std::filesystem::is_directory(folder, code)) {
            continue;
        }
        std::filesystem::recursive_directory_iterator it(
            folder, std::filesystem::directory_options::skip_permission_denied, code);
        const std::filesystem::recursive_directory_iterator last;
        for (; !code && it != last; it.increment(code)) {
            std::error_code type_code;
            if (!it->is_regular_file(type_code) || it->path().extension() != ".clap") {
                continue;
            }
            if (!seen.insert(std::filesystem::weakly_canonical(it->path(), type_code)).second) {
                continue;  // the same file through another folder
            }
            std::string error;
            std::vector<PluginInfo> infos = read_clap_file(it->path(), &error);
            if (infos.empty() && problems != nullptr) {
                problems->push_back(error);
            }
            result.insert(result.end(), infos.begin(), infos.end());
        }
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const PluginInfo& a, const PluginInfo& b) { return a.name < b.name; });
    return result;
}

// ---- A plugin ----------------------------------------------------------------------------

ClapPlugin::ClapPlugin() : impl_(std::make_unique<Impl>()) {}

std::shared_ptr<ClapPlugin> ClapPlugin::create(const std::filesystem::path& file, const std::string& id,
                                               double sample_rate, std::uint32_t channels,
                                               std::string* error) {
    std::shared_ptr<Library> library = Library::open(file, error);
    if (!library) {
        return nullptr;
    }
    std::shared_ptr<ClapPlugin> result(new ClapPlugin());
    Impl& s = *result->impl_;
    s.library = library;
    s.main_thread = std::this_thread::get_id();
    s.host.clap_version = CLAP_VERSION;
    s.host.host_data = &s;
    s.host.name = "Tone of Hell";
    s.host.vendor = "AmmaArk";
    s.host.url = "https://github.com/AmmaArkf/Tone-of-Hell";
    s.host.version = "0.2";
    s.host.get_extension = &host_extension;
    s.host.request_restart = [](const clap_host_t*) {};
    s.host.request_process = [](const clap_host_t*) {};
    s.host.request_callback = [](const clap_host_t* host) { impl_of(host).callback_requested = true; };

    const clap_plugin_factory_t* factory = library->factory();
    s.plugin = factory->create_plugin(factory, &s.host, id.c_str());
    if (s.plugin == nullptr) {
        set_error(error, file.u8string() + " has no plugin " + id);
        return nullptr;
    }
    if (!s.plugin->init(s.plugin)) {
        s.plugin->destroy(s.plugin);
        s.plugin = nullptr;
        set_error(error, id + " did not start");
        return nullptr;
    }
    s.info = describe(*s.plugin->desc, file);
    s.params = static_cast<const clap_plugin_params_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_PARAMS));
    s.state = static_cast<const clap_plugin_state_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_STATE));
    s.gui = static_cast<const clap_plugin_gui_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_GUI));
    const auto* ports =
        static_cast<const clap_plugin_audio_ports_t*>(s.plugin->get_extension(s.plugin, CLAP_EXT_AUDIO_PORTS));

    // The main audio ports. Without the extension the plugin gets as many channels as we have.
    const std::uint32_t own = std::clamp<std::uint32_t>(channels, 1, kMaxChannels);
    s.in_channels = own;
    s.out_channels = own;
    if (ports != nullptr) {
        s.in_channels = 0;
        if (ports->count(s.plugin, true) > 0) {
            clap_audio_port_info_t port{};
            s.in_channels = ports->get(s.plugin, 0, true, &port) ? port.channel_count : own;
        }
        if (ports->count(s.plugin, false) == 0) {
            set_error(error, s.info.name + " has no audio output");
            return nullptr;
        }
        clap_audio_port_info_t port{};
        s.out_channels = ports->get(s.plugin, 0, false, &port) ? port.channel_count : own;
        s.in_channels = std::min(s.in_channels, kMaxChannels);
        s.out_channels = std::clamp<std::uint32_t>(s.out_channels, 1, kMaxChannels);
    }
    s.in_buffer.assign(static_cast<std::size_t>(kMaxFrames) * kMaxChannels, 0.0f);
    s.out_buffer.assign(static_cast<std::size_t>(kMaxFrames) * kMaxChannels, 0.0f);
    for (std::size_t c = 0; c < kMaxChannels; ++c) {
        s.in_ptrs[c] = s.in_buffer.data() + c * kMaxFrames;
        s.out_ptrs[c] = s.out_buffer.data() + c * kMaxFrames;
    }
    load_params(s);
    if (!s.plugin->activate(s.plugin, sample_rate, 1, kMaxFrames)) {
        set_error(error, s.info.name + " could not get ready to process sound");
        return nullptr;
    }
    s.active = true;
    return result;
}

ClapPlugin::~ClapPlugin() {
    Impl& s = *impl_;
    if (s.plugin == nullptr) {
        return;
    }
    close_gui();
    if (s.processing) {
        // The engine no longer uses the plugin: the main thread ends its processing.
        s.ending = true;
        s.plugin->stop_processing(s.plugin);
        s.processing = false;
        s.ending = false;
    }
    if (s.active) {
        s.plugin->deactivate(s.plugin);
    }
    s.plugin->destroy(s.plugin);
    s.plugin = nullptr;
}

const PluginInfo& ClapPlugin::info() const noexcept { return impl_->info; }

void ClapPlugin::process(float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept {
    Impl& s = *impl_;
    t_audio_thread = true;
    if (!s.active || channels == 0 || interleaved == nullptr) {
        return;
    }
    if (!s.processing) {
        if (!s.plugin->start_processing(s.plugin)) {
            return;
        }
        s.processing = true;
    }

    // New parameter values go to the plugin as events at the start of the block.
    s.event_count = 0;
    ParamChange change;
    std::uint64_t popped = 0;
    std::uint32_t param_count = 0;
    while (param_count < s.events.size() && s.changes.try_pop(change)) {
        clap_event_param_value_t& event = s.events[param_count++];
        s.order[s.event_count++] = &event.header;
        event = clap_event_param_value_t{};
        event.header.size = static_cast<std::uint32_t>(sizeof(clap_event_param_value_t));
        event.header.time = 0;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = CLAP_EVENT_PARAM_VALUE;
        event.header.flags = 0;
        event.param_id = change.id;
        event.cookie = nullptr;
        event.note_id = -1;
        event.port_index = -1;
        event.channel = -1;
        event.key = -1;
        event.value = change.value;
        ++popped;
    }
    if (popped > 0) {
        s.consumed.fetch_add(popped, std::memory_order_release);
    }
    // Notes, sorted by their frame (the engine adds them track by track, region by region).
    for (std::uint32_t i = 1; i < s.pending_count; ++i) {
        const PendingNote note = s.pending[i];
        std::uint32_t j = i;
        while (j > 0 && s.pending[j - 1].frame > note.frame) {
            s.pending[j] = s.pending[j - 1];
            --j;
        }
        s.pending[j] = note;
    }
    for (std::uint32_t i = 0; i < s.pending_count; ++i) {
        const PendingNote& note = s.pending[i];
        clap_event_note_t& event = s.note_events[i];
        event = clap_event_note_t{};
        event.header.size = static_cast<std::uint32_t>(sizeof(clap_event_note_t));
        event.header.time = frames > 0 ? std::min(note.frame, frames - 1) : 0;
        event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        event.header.type = note.on ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF;
        event.header.flags = 0;
        event.note_id = -1;
        event.port_index = note.key < 0 ? -1 : 0;  // -1: every port, channel and key
        event.channel = note.key < 0 ? -1 : 0;
        event.key = note.key;
        event.velocity = static_cast<double>(note.velocity);
        s.order[s.event_count++] = &event.header;
    }
    s.pending_count = 0;

    const clap_input_events_t in_events{&s, &events_size, &events_get};
    const clap_output_events_t out_events{nullptr, &events_push};
    for (std::uint32_t done = 0; done < frames;) {
        const std::uint32_t count = std::min(frames - done, kMaxFrames);
        float* block = interleaved + static_cast<std::size_t>(done) * channels;
        for (std::uint32_t c = 0; c < s.in_channels; ++c) {
            const std::uint32_t from = std::min(c, channels - 1);
            float* destination = s.in_ptrs[c];
            for (std::uint32_t i = 0; i < count; ++i) {
                destination[i] = block[static_cast<std::size_t>(i) * channels + from];
            }
        }
        clap_audio_buffer_t input{};
        input.data32 = s.in_ptrs.data();
        input.channel_count = s.in_channels;
        clap_audio_buffer_t output{};
        output.data32 = s.out_ptrs.data();
        output.channel_count = s.out_channels;
        clap_process_t call{};
        call.steady_time = s.steady_time;
        call.frames_count = count;
        call.transport = nullptr;
        call.audio_inputs = s.in_channels > 0 ? &input : nullptr;
        call.audio_inputs_count = s.in_channels > 0 ? 1 : 0;
        call.audio_outputs = &output;
        call.audio_outputs_count = 1;
        call.in_events = &in_events;
        call.out_events = &out_events;
        const clap_process_status status = s.plugin->process(s.plugin, &call);
        s.event_count = 0;  // the events belonged to the first part
        s.steady_time += count;
        if (status != CLAP_PROCESS_ERROR) {
            for (std::uint32_t c = 0; c < channels; ++c) {
                const float* source = s.out_ptrs[std::min(c, s.out_channels - 1)];
                for (std::uint32_t i = 0; i < count; ++i) {
                    block[static_cast<std::size_t>(i) * channels + c] = source[i];
                }
            }
        }
        done += count;
    }
}

void ClapPlugin::add_note(std::uint32_t frame, bool on, int pitch, float velocity) noexcept {
    Impl& s = *impl_;
    if (s.pending_count >= s.pending.size() || pitch > 127 || (on && pitch < 0)) {
        return;
    }
    PendingNote& note = s.pending[s.pending_count++];
    note.frame = frame;
    note.on = on;
    note.key = static_cast<std::int16_t>(std::max(pitch, -1));
    note.velocity = velocity;
}

bool ClapPlugin::is_instrument() const noexcept { return impl_->info.instrument || impl_->in_channels == 0; }

const std::vector<ClapPlugin::Param>& ClapPlugin::params() const noexcept { return impl_->param_list; }

double ClapPlugin::param_value(std::uint32_t id) const {
    const Impl& s = *impl_;
    const auto found = s.requested.find(id);
    if (found != s.requested.end()) {
        return found->second.first;  // sent, but the plugin has not had it yet
    }
    double value = 0.0;
    if (s.params != nullptr && s.params->get_value(s.plugin, id, &value)) {
        return value;
    }
    for (const Param& param : s.param_list) {
        if (param.id == id) {
            return param.default_value;
        }
    }
    return 0.0;
}

std::string ClapPlugin::param_text(std::uint32_t id, double value) const {
    const Impl& s = *impl_;
    char text[256] = {};
    if (s.params != nullptr && s.params->value_to_text != nullptr &&
        s.params->value_to_text(s.plugin, id, value, text, static_cast<std::uint32_t>(sizeof(text))) &&
        text[0] != '\0') {
        return text;
    }
    std::snprintf(text, sizeof(text), "%.3g", value);
    return text;
}

bool ClapPlugin::set_param(std::uint32_t id, double value) {
    Impl& s = *impl_;
    if (!s.changes.try_push(ParamChange{id, value})) {
        return false;
    }
    ++s.sent;
    s.requested[id] = {value, s.sent};
    return true;
}

bool ClapPlugin::save_state(std::vector<char>& state) const {
    const Impl& s = *impl_;
    state.clear();
    if (s.state == nullptr) {
        return false;
    }
    clap_ostream_t stream{};
    stream.ctx = &state;
    stream.write = [](const clap_ostream_t* out, const void* buffer, std::uint64_t size) -> std::int64_t {
        auto* bytes = static_cast<std::vector<char>*>(out->ctx);
        const auto* from = static_cast<const char*>(buffer);
        bytes->insert(bytes->end(), from, from + static_cast<std::size_t>(size));
        return static_cast<std::int64_t>(size);
    };
    return s.state->save(s.plugin, &stream);
}

bool ClapPlugin::load_state(const std::vector<char>& state) {
    Impl& s = *impl_;
    if (s.state == nullptr) {
        return false;
    }
    struct Reader {
        const std::vector<char>* bytes;
        std::size_t position;
    } reader{&state, 0};
    clap_istream_t stream{};
    stream.ctx = &reader;
    stream.read = [](const clap_istream_t* in, void* buffer, std::uint64_t size) -> std::int64_t {
        auto* r = static_cast<Reader*>(in->ctx);
        const std::size_t left = r->bytes->size() - r->position;
        const std::size_t count = std::min(left, static_cast<std::size_t>(size));
        if (count > 0) {
            std::memcpy(buffer, r->bytes->data() + r->position, count);
        }
        r->position += count;
        return static_cast<std::int64_t>(count);
    };
    const bool loaded = s.state->load(s.plugin, &stream);
    if (loaded) {
        s.requested.clear();
        load_params(s);
    }
    return loaded;
}

bool ClapPlugin::has_gui() const {
    const Impl& s = *impl_;
    return s.gui != nullptr && s.gui->is_api_supported(s.plugin, kWindowApi, false);
}

bool ClapPlugin::gui_open() const noexcept { return impl_->gui_created; }

bool ClapPlugin::open_gui(void* parent, std::uint32_t& width, std::uint32_t& height, bool& resizable) {
    Impl& s = *impl_;
    if (!has_gui() || parent == nullptr) {
        return false;
    }
    close_gui();
    if (!s.gui->create(s.plugin, kWindowApi, false)) {
        return false;
    }
    s.gui_created = true;
    s.gui_closed = false;
    s.gui->set_scale(s.plugin, 1.0);
    resizable = s.gui->can_resize(s.plugin);
    if (!s.gui->get_size(s.plugin, &width, &height) || width == 0 || height == 0) {
        width = 640;
        height = 400;
    }
    clap_window_t window{};
    window.api = kWindowApi;
#ifdef _WIN32
    window.win32 = parent;
#else
    window.x11 = static_cast<clap_xwnd>(reinterpret_cast<std::uintptr_t>(parent));
#endif
    if (!s.gui->set_parent(s.plugin, &window)) {
        close_gui();
        return false;
    }
    s.gui->show(s.plugin);
    return true;
}

bool ClapPlugin::resize_gui(std::uint32_t& width, std::uint32_t& height) {
    Impl& s = *impl_;
    if (!s.gui_created || !s.gui->can_resize(s.plugin)) {
        return false;
    }
    s.gui->adjust_size(s.plugin, &width, &height);
    return s.gui->set_size(s.plugin, width, height);
}

void ClapPlugin::close_gui() {
    Impl& s = *impl_;
    if (!s.gui_created) {
        return;
    }
    s.gui->hide(s.plugin);
    s.gui->destroy(s.plugin);
    s.gui_created = false;
}

bool ClapPlugin::take_resize_request(std::uint32_t& width, std::uint32_t& height) {
    Impl& s = *impl_;
    if (!s.resize_requested.exchange(false)) {
        return false;
    }
    width = s.resize_width;
    height = s.resize_height;
    return width > 0 && height > 0;
}

bool ClapPlugin::take_gui_closed() { return impl_->gui_closed.exchange(false); }

void ClapPlugin::idle() {
    Impl& s = *impl_;
    if (s.plugin == nullptr) {
        return;
    }
    if (s.callback_requested.exchange(false)) {
        s.plugin->on_main_thread(s.plugin);
    }
    if (s.rescan_requested.exchange(false)) {
        load_params(s);
    }
    const std::uint64_t consumed = s.consumed.load(std::memory_order_acquire);
    for (auto it = s.requested.begin(); it != s.requested.end();) {
        if (it->second.second <= consumed) {
            it = s.requested.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace toh::plugins
