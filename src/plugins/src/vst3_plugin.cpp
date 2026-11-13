#include "toh/plugins/vst3_plugin.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstattributes.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivsthostapplication.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "toh/core/spsc_queue.h"
#include "toh/plugins/clap_plugin.h"

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

using namespace Steinberg;

namespace {

constexpr std::uint32_t kMaxFrames = 4096;
constexpr std::uint32_t kMaxChannels = 8;

void set_error(std::string* error, const std::string& text) {
    if (error != nullptr) {
        *error = text;
    }
}

bool same_iid(const TUID a, const FUID& b) { return FUnknownPrivate::iidEqual(a, b.toTUID()); }

std::string hex_id(const TUID id) {
    static const char* const digits = "0123456789ABCDEF";
    std::string text;
    for (int i = 0; i < 16; ++i) {
        const auto byte = static_cast<unsigned char>(id[i]);
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

std::string utf8(const char16* text) {
    std::string result;
    if (text == nullptr) {
        return result;
    }
    for (std::size_t i = 0; text[i] != 0; ++i) {
        std::uint32_t code = static_cast<std::uint32_t>(text[i]);
        if (code >= 0xD800 && code < 0xDC00 && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000) {
            code = 0x10000 + ((code - 0xD800) << 10) + (static_cast<std::uint32_t>(text[i + 1]) - 0xDC00);
            ++i;
        }
        if (code < 0x80) {
            result += static_cast<char>(code);
        } else if (code < 0x800) {
            result += static_cast<char>(0xC0 | (code >> 6));
            result += static_cast<char>(0x80 | (code & 0x3F));
        } else if (code < 0x10000) {
            result += static_cast<char>(0xE0 | (code >> 12));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            result += static_cast<char>(0xF0 | (code >> 18));
            result += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            result += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            result += static_cast<char>(0x80 | (code & 0x3F));
        }
    }
    return result;
}

std::string text_of(const char8* text, std::size_t size) {
    std::size_t length = 0;
    while (length < size && text[length] != 0) {
        ++length;
    }
    return std::string(text, length);
}

// ---- Host objects --------------------------------------------------------------------------
// Objects that live as long as the plugin; reference counting is not needed for them.

template <typename Interface>
class Unowned : public Interface {
public:
    tresult PLUGIN_API queryInterface(const TUID wanted, void** object) override {
        if (same_iid(wanted, FUnknown::iid) || same_iid(wanted, Interface::iid)) {
            *object = static_cast<Interface*>(this);
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }
};

// Objects the plugin may keep: counted, and deleted with the last reference.
template <typename Interface>
class Counted : public Interface {
public:
    virtual ~Counted() = default;
    tresult PLUGIN_API queryInterface(const TUID wanted, void** object) override {
        if (same_iid(wanted, FUnknown::iid) || same_iid(wanted, Interface::iid)) {
            *object = static_cast<Interface*>(this);
            addRef();
            return kResultOk;
        }
        *object = nullptr;
        return kNoInterface;
    }
    uint32 PLUGIN_API addRef() override { return ++references_; }
    uint32 PLUGIN_API release() override {
        const uint32 left = --references_;
        if (left == 0) {
            delete this;
        }
        return left;
    }

private:
    std::atomic<uint32> references_{1};
};

class Attributes final : public Counted<Vst::IAttributeList> {
public:
    tresult PLUGIN_API setInt(AttrID id, int64 value) override {
        ints_[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getInt(AttrID id, int64& value) override {
        const auto found = ints_.find(id);
        if (found == ints_.end()) {
            return kResultFalse;
        }
        value = found->second;
        return kResultOk;
    }
    tresult PLUGIN_API setFloat(AttrID id, double value) override {
        floats_[id] = value;
        return kResultOk;
    }
    tresult PLUGIN_API getFloat(AttrID id, double& value) override {
        const auto found = floats_.find(id);
        if (found == floats_.end()) {
            return kResultFalse;
        }
        value = found->second;
        return kResultOk;
    }
    tresult PLUGIN_API setString(AttrID id, const Vst::TChar* string) override {
        std::u16string text;
        for (std::size_t i = 0; string != nullptr && string[i] != 0; ++i) {
            text += string[i];
        }
        strings_[id] = text;
        return kResultOk;
    }
    tresult PLUGIN_API getString(AttrID id, Vst::TChar* string, uint32 size_in_bytes) override {
        const auto found = strings_.find(id);
        if (found == strings_.end() || size_in_bytes < sizeof(Vst::TChar)) {
            return kResultFalse;
        }
        const std::size_t room = size_in_bytes / sizeof(Vst::TChar) - 1;
        const std::size_t count = std::min(room, found->second.size());
        std::copy(found->second.begin(), found->second.begin() + static_cast<std::ptrdiff_t>(count), string);
        string[count] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API setBinary(AttrID id, const void* data, uint32 size_in_bytes) override {
        const auto* bytes = static_cast<const char*>(data);
        binaries_[id] = std::vector<char>(bytes, bytes + size_in_bytes);
        return kResultOk;
    }
    tresult PLUGIN_API getBinary(AttrID id, const void*& data, uint32& size_in_bytes) override {
        const auto found = binaries_.find(id);
        if (found == binaries_.end()) {
            return kResultFalse;
        }
        data = found->second.data();
        size_in_bytes = static_cast<uint32>(found->second.size());
        return kResultOk;
    }

private:
    std::map<std::string, int64> ints_;
    std::map<std::string, double> floats_;
    std::map<std::string, std::u16string> strings_;
    std::map<std::string, std::vector<char>> binaries_;
};

class Message final : public Counted<Vst::IMessage> {
public:
    Message() : attributes_(new Attributes()) {}
    ~Message() override { attributes_->release(); }
    FIDString PLUGIN_API getMessageID() override { return id_.c_str(); }
    void PLUGIN_API setMessageID(FIDString id) override { id_ = id != nullptr ? id : ""; }
    Vst::IAttributeList* PLUGIN_API getAttributes() override { return attributes_; }

private:
    std::string id_;
    Attributes* attributes_;
};

class HostApplication final : public Unowned<Vst::IHostApplication> {
public:
    tresult PLUGIN_API getName(Vst::String128 name) override {
        const char16_t* text = u"Tone of Hell";
        std::size_t i = 0;
        for (; text[i] != 0 && i < 127; ++i) {
            name[i] = text[i];
        }
        name[i] = 0;
        return kResultOk;
    }
    tresult PLUGIN_API createInstance(TUID cid, TUID wanted, void** object) override {
        if (same_iid(cid, Vst::IMessage::iid) && same_iid(wanted, Vst::IMessage::iid)) {
            *object = static_cast<Vst::IMessage*>(new Message());
            return kResultTrue;
        }
        if (same_iid(cid, Vst::IAttributeList::iid) && same_iid(wanted, Vst::IAttributeList::iid)) {
            *object = static_cast<Vst::IAttributeList*>(new Attributes());
            return kResultTrue;
        }
        *object = nullptr;
        return kResultFalse;
    }
};

// A stream in memory, for the states of the component and the controller.
class MemoryStream final : public Unowned<IBStream> {
public:
    explicit MemoryStream(std::vector<char>& bytes) : bytes_(bytes) {}
    tresult PLUGIN_API read(void* buffer, int32 count, int32* read) override {
        const std::size_t left = bytes_.size() - std::min(bytes_.size(), position_);
        const std::size_t n = std::min(left, static_cast<std::size_t>(std::max(count, 0)));
        if (n > 0) {
            std::memcpy(buffer, bytes_.data() + position_, n);
        }
        position_ += n;
        if (read != nullptr) {
            *read = static_cast<int32>(n);
        }
        return kResultOk;
    }
    tresult PLUGIN_API write(void* buffer, int32 count, int32* written) override {
        if (count < 0) {
            return kInvalidArgument;
        }
        const auto n = static_cast<std::size_t>(count);
        if (position_ + n > bytes_.size()) {
            bytes_.resize(position_ + n);
        }
        std::memcpy(bytes_.data() + position_, buffer, n);
        position_ += n;
        if (written != nullptr) {
            *written = count;
        }
        return kResultOk;
    }
    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override {
        int64 base = 0;
        if (mode == kIBSeekCur) {
            base = static_cast<int64>(position_);
        } else if (mode == kIBSeekEnd) {
            base = static_cast<int64>(bytes_.size());
        }
        const int64 target = base + pos;
        if (target < 0) {
            return kResultFalse;
        }
        position_ = static_cast<std::size_t>(target);
        if (result != nullptr) {
            *result = target;
        }
        return kResultOk;
    }
    tresult PLUGIN_API tell(int64* pos) override {
        if (pos != nullptr) {
            *pos = static_cast<int64>(position_);
        }
        return kResultOk;
    }

private:
    std::vector<char>& bytes_;
    std::size_t position_ = 0;
};

// Parameter values for one block: preallocated, so the audio thread never allocates.
class ValueQueue final : public Unowned<Vst::IParamValueQueue> {
public:
    void reset(Vst::ParamID id) {
        id_ = id;
        count_ = 0;
    }
    Vst::ParamID PLUGIN_API getParameterId() override { return id_; }
    int32 PLUGIN_API getPointCount() override { return count_; }
    tresult PLUGIN_API getPoint(int32 index, int32& offset, Vst::ParamValue& value) override {
        if (index < 0 || index >= count_) {
            return kResultFalse;
        }
        offset = points_[static_cast<std::size_t>(index)].first;
        value = points_[static_cast<std::size_t>(index)].second;
        return kResultOk;
    }
    tresult PLUGIN_API addPoint(int32 offset, Vst::ParamValue value, int32& index) override {
        if (count_ > 0 && points_[static_cast<std::size_t>(count_ - 1)].first == offset) {
            points_[static_cast<std::size_t>(count_ - 1)].second = value;  // the same moment again
            index = count_ - 1;
            return kResultOk;
        }
        if (count_ >= static_cast<int32>(points_.size())) {
            return kResultFalse;
        }
        points_[static_cast<std::size_t>(count_)] = {offset, value};
        index = count_++;
        return kResultOk;
    }

private:
    Vst::ParamID id_ = 0;
    int32 count_ = 0;
    std::array<std::pair<int32, Vst::ParamValue>, 16> points_{};
};

class ParameterChanges final : public Unowned<Vst::IParameterChanges> {
public:
    void clear() { count_ = 0; }
    int32 PLUGIN_API getParameterCount() override { return count_; }
    Vst::IParamValueQueue* PLUGIN_API getParameterData(int32 index) override {
        return index >= 0 && index < count_ ? &queues_[static_cast<std::size_t>(index)] : nullptr;
    }
    Vst::IParamValueQueue* PLUGIN_API addParameterData(const Vst::ParamID& id, int32& index) override {
        for (int32 i = 0; i < count_; ++i) {
            if (queues_[static_cast<std::size_t>(i)].getParameterId() == id) {
                index = i;
                return &queues_[static_cast<std::size_t>(i)];
            }
        }
        if (count_ >= static_cast<int32>(queues_.size())) {
            return nullptr;
        }
        index = count_;
        ValueQueue& queue = queues_[static_cast<std::size_t>(count_++)];
        queue.reset(id);
        return &queue;
    }

private:
    std::array<ValueQueue, 64> queues_{};
    int32 count_ = 0;
};

class EventList final : public Unowned<Vst::IEventList> {
public:
    void clear() { count_ = 0; }
    int32 PLUGIN_API getEventCount() override { return count_; }
    tresult PLUGIN_API getEvent(int32 index, Vst::Event& event) override {
        if (index < 0 || index >= count_) {
            return kResultFalse;
        }
        event = events_[static_cast<std::size_t>(index)];
        return kResultOk;
    }
    tresult PLUGIN_API addEvent(Vst::Event& event) override {
        if (count_ >= static_cast<int32>(events_.size())) {
            return kResultFalse;
        }
        events_[static_cast<std::size_t>(count_++)] = event;
        return kResultOk;
    }

private:
    std::array<Vst::Event, 512> events_{};
    int32 count_ = 0;
};

// A loaded .vst3 module. One module is loaded once however many of its plugins are in use.
class Module {
public:
    static std::shared_ptr<Module> open(const std::filesystem::path& bundle, std::string* error);
    ~Module();
    Module(const Module&) = delete;
    Module& operator=(const Module&) = delete;
    IPluginFactory* factory() const noexcept { return factory_; }

private:
    Module() = default;
#ifdef _WIN32
    HMODULE module_ = nullptr;
#else
    void* module_ = nullptr;
#endif
    IPluginFactory* factory_ = nullptr;
    bool entered_ = false;
};

template <typename Function>
Function symbol(
#ifdef _WIN32
    HMODULE module,
#else
    void* module,
#endif
    const char* name) {
    Function function = nullptr;
#ifdef _WIN32
    const FARPROC found = GetProcAddress(module, name);
#else
    void* found = dlsym(module, name);
#endif
    static_assert(sizeof(found) == sizeof(function), "a symbol is a pointer");
    if (found != nullptr) {
        std::memcpy(&function, &found, sizeof(function));
    }
    return function;
}

// The library inside a bundle: Contents/x86_64-win/Name.vst3 or Contents/x86_64-linux/Name.so.
std::filesystem::path module_file(const std::filesystem::path& bundle) {
    std::error_code code;
    if (std::filesystem::is_regular_file(bundle, code)) {
        return bundle;  // an old single-file plugin
    }
#ifdef _WIN32
    const std::filesystem::path file = bundle / "Contents" / "x86_64-win" / bundle.filename();
#else
    std::filesystem::path name = bundle.filename();
    name.replace_extension(".so");
    const std::filesystem::path file = bundle / "Contents" / "x86_64-linux" / name;
#endif
    return std::filesystem::is_regular_file(file, code) ? file : std::filesystem::path();
}

std::map<std::filesystem::path, std::weak_ptr<Module>>& loaded_modules() {
    static std::map<std::filesystem::path, std::weak_ptr<Module>> modules;
    return modules;
}

std::shared_ptr<Module> Module::open(const std::filesystem::path& bundle, std::string* error) {
    const std::filesystem::path file = module_file(bundle);
    if (file.empty()) {
        set_error(error, bundle.u8string() + " has no library for this system");
        return nullptr;
    }
    auto& modules = loaded_modules();
    const auto found = modules.find(file);
    if (found != modules.end()) {
        if (auto alive = found->second.lock()) {
            return alive;
        }
    }
    std::shared_ptr<Module> module(new Module());
#ifdef _WIN32
    module->module_ = LoadLibraryW(file.wstring().c_str());
    if (module->module_ == nullptr) {
        set_error(error, "cannot load " + file.u8string());
        return nullptr;
    }
    if (auto init = symbol<bool (PLUGIN_API*)()>(module->module_, "InitDll")) {
        if (!init()) {
            set_error(error, file.u8string() + " did not start");
            return nullptr;
        }
    }
    module->entered_ = true;
#else
    module->module_ = dlopen(file.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (module->module_ == nullptr) {
        const char* reason = dlerror();
        set_error(error, "cannot load " + file.u8string() + (reason != nullptr ? std::string(": ") + reason : ""));
        return nullptr;
    }
    if (auto entry = symbol<bool (*)(void*)>(module->module_, "ModuleEntry")) {
        if (!entry(module->module_)) {
            set_error(error, file.u8string() + " did not start");
            return nullptr;
        }
    }
    module->entered_ = true;
#endif
    auto get_factory = symbol<IPluginFactory* (PLUGIN_API*)()>(module->module_, "GetPluginFactory");
    module->factory_ = get_factory != nullptr ? get_factory() : nullptr;
    if (module->factory_ == nullptr) {
        set_error(error, file.u8string() + " is not a VST3 plugin (it has no factory)");
        return nullptr;
    }
    modules[file] = module;
    return module;
}

Module::~Module() {
    if (factory_ != nullptr) {
        factory_->release();
    }
#ifdef _WIN32
    if (module_ != nullptr) {
        if (entered_) {
            if (auto exit = symbol<bool (PLUGIN_API*)()>(module_, "ExitDll")) {
                exit();
            }
        }
        FreeLibrary(module_);
    }
#else
    if (module_ != nullptr) {
        if (entered_) {
            if (auto exit = symbol<bool (*)()>(module_, "ModuleExit")) {
                exit();
            }
        }
        dlclose(module_);
    }
#endif
}

// The audio plugin classes of a factory, with what is known about them.
struct ClassEntry {
    PluginInfo info;
    TUID cid{};
};

std::vector<ClassEntry> audio_classes(IPluginFactory* factory, const std::filesystem::path& bundle) {
    std::vector<ClassEntry> classes;
    PFactoryInfo factory_info;
    std::string factory_vendor;
    if (factory->getFactoryInfo(&factory_info) == kResultOk) {
        factory_vendor = text_of(factory_info.vendor, PFactoryInfo::kNameSize);
    }
    IPluginFactory2* factory2 = nullptr;
    if (factory->queryInterface(IPluginFactory2::iid, reinterpret_cast<void**>(&factory2)) != kResultOk) {
        factory2 = nullptr;
    }
    const int32 count = factory->countClasses();
    for (int32 i = 0; i < count; ++i) {
        PClassInfo info;
        if (factory->getClassInfo(i, &info) != kResultOk ||
            std::strncmp(info.category, kVstAudioEffectClass, PClassInfo::kCategorySize) != 0) {
            continue;
        }
        ClassEntry entry;
        std::memcpy(entry.cid, info.cid, sizeof(TUID));
        entry.info.format = "vst3";
        entry.info.path = bundle;
        entry.info.id = hex_id(info.cid);
        entry.info.name = text_of(info.name, PClassInfo::kNameSize);
        entry.info.vendor = factory_vendor;
        PClassInfo2 info2;
        if (factory2 != nullptr && factory2->getClassInfo2(i, &info2) == kResultOk) {
            const std::string vendor = text_of(info2.vendor, PClassInfo2::kVendorSize);
            if (!vendor.empty()) {
                entry.info.vendor = vendor;
            }
            entry.info.version = text_of(info2.version, PClassInfo2::kVersionSize);
            const std::string sub = text_of(info2.subCategories, PClassInfo2::kSubCategoriesSize);
            entry.info.description = sub;
            entry.info.instrument = sub.find("Instrument") != std::string::npos;
        }
        classes.push_back(std::move(entry));
    }
    if (factory2 != nullptr) {
        factory2->release();
    }
    return classes;
}

}  // namespace

// ---- Finding plugins ---------------------------------------------------------------------

std::vector<std::filesystem::path> default_vst3_folders() {
    std::vector<std::filesystem::path> folders;
#ifdef _WIN32
    const auto environment = [](const wchar_t* name) {
        std::vector<wchar_t> buffer(32768);
        const DWORD length = GetEnvironmentVariableW(name, buffer.data(), static_cast<DWORD>(buffer.size()));
        return length > 0 && length < buffer.size() ? std::filesystem::path(std::wstring(buffer.data(), length))
                                                    : std::filesystem::path();
    };
    const std::filesystem::path common = environment(L"COMMONPROGRAMFILES");
    if (!common.empty()) {
        folders.push_back(common / "VST3");
    }
    const std::filesystem::path local = environment(L"LOCALAPPDATA");
    if (!local.empty()) {
        folders.push_back(local / "Programs" / "Common" / "VST3");
    }
#else
    if (const char* home = std::getenv("HOME")) {
        folders.push_back(std::filesystem::path(home) / ".vst3");
    }
    folders.emplace_back("/usr/lib/vst3");
    folders.emplace_back("/usr/local/lib/vst3");
#endif
    return folders;
}

std::vector<PluginInfo> read_vst3_file(const std::filesystem::path& bundle, std::string* error) {
    const std::shared_ptr<Module> module = Module::open(bundle, error);
    if (!module) {
        return {};
    }
    std::vector<PluginInfo> infos;
    for (ClassEntry& entry : audio_classes(module->factory(), bundle)) {
        infos.push_back(std::move(entry.info));
    }
    if (infos.empty()) {
        set_error(error, bundle.u8string() + " contains no audio plugins");
    }
    return infos;
}

std::vector<PluginInfo> scan_vst3_folders(const std::vector<std::filesystem::path>& folders,
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
            if (it->path().extension() != ".vst3") {
                continue;
            }
            std::error_code type_code;
            if (it->is_directory(type_code)) {
                it.disable_recursion_pending();  // a bundle: its library is found inside
            }
            if (!seen.insert(std::filesystem::weakly_canonical(it->path(), type_code)).second) {
                continue;
            }
            std::string error;
            std::vector<PluginInfo> infos = read_vst3_file(it->path(), &error);
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

namespace {

struct ParamChange {
    Vst::ParamID id = 0;
    double value = 0.0;  // normalized
};

}  // namespace

namespace {

// The plugin's controller reports edits made in its own window here.
class ComponentHandler final : public Unowned<Vst::IComponentHandler> {
public:
    explicit ComponentHandler(Vst3Plugin::Impl& owner) : owner_(owner) {}
    tresult PLUGIN_API beginEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue value) override;
    tresult PLUGIN_API endEdit(Vst::ParamID) override { return kResultOk; }
    tresult PLUGIN_API restartComponent(int32 flags) override;

private:
    Vst3Plugin::Impl& owner_;
};

class PlugFrame final : public Unowned<IPlugFrame> {
public:
    explicit PlugFrame(Vst3Plugin::Impl& owner) : owner_(owner) {}
    tresult PLUGIN_API resizeView(IPlugView* view, ViewRect* size) override;

private:
    Vst3Plugin::Impl& owner_;
};

}  // namespace

struct Vst3Plugin::Impl {
    std::shared_ptr<Module> module;
    PluginInfo info;
    HostApplication host;
    ComponentHandler handler{*this};
    PlugFrame frame{*this};
    Vst::IComponent* component = nullptr;
    Vst::IAudioProcessor* processor = nullptr;
    Vst::IEditController* controller = nullptr;
    bool separate_controller = false;
    Vst::IConnectionPoint* component_point = nullptr;
    Vst::IConnectionPoint* controller_point = nullptr;
    IPlugView* view = nullptr;

    std::vector<Param> param_list;
    std::vector<double> scale;  // per parameter: the plain range (steps + 1, or 1)
    std::int32_t in_buses = 0;
    std::int32_t out_buses = 0;
    std::uint32_t in_channels = 2;
    std::uint32_t out_channels = 2;
    bool event_input = false;
    bool active = false;
    std::atomic<bool> processing{false};
    std::uint32_t latency = 0;
    std::atomic<bool> latency_changed{false};
    std::atomic<bool> rescan_requested{false};
    std::atomic<bool> resize_requested{false};
    std::atomic<std::uint32_t> resize_width{0};
    std::atomic<std::uint32_t> resize_height{0};
    double sample_rate = 48000.0;

    // Audio thread.
    std::vector<float> in_buffer;
    std::vector<float> out_buffer;
    std::vector<float> spare_buffer;  // for the buses other than the main one
    std::array<float*, kMaxChannels> in_ptrs{};
    std::array<float*, kMaxChannels> out_ptrs{};
    std::array<float*, kMaxChannels> spare_ptrs{};
    std::vector<Vst::AudioBusBuffers> inputs;
    std::vector<Vst::AudioBusBuffers> outputs;
    ParameterChanges in_changes;
    ParameterChanges out_changes;
    EventList in_events;
    EventList out_events;
    Vst::ProcessContext context{};
    std::array<bool, 128> held{};  // the keys that are down
    std::int64_t time = 0;

    core::SpscQueue<ParamChange, 1024> changes;  // to the audio thread
    std::uint64_t sent = 0;
    std::atomic<std::uint64_t> consumed{0};
    std::map<std::uint32_t, std::pair<double, std::uint64_t>> requested;  // plain value, change number
};

namespace {

tresult PLUGIN_API ComponentHandler::performEdit(Vst::ParamID id, Vst::ParamValue value) {
    owner_.changes.try_push(ParamChange{id, value});  // the plugin's own window changed it
    return kResultOk;
}

tresult PLUGIN_API ComponentHandler::restartComponent(int32 flags) {
    if ((flags & Vst::kLatencyChanged) != 0) {
        owner_.latency_changed = true;
    }
    if ((flags & (Vst::kParamValuesChanged | Vst::kParamTitlesChanged)) != 0) {
        owner_.rescan_requested = true;
    }
    return kResultOk;
}

tresult PLUGIN_API PlugFrame::resizeView(IPlugView*, ViewRect* size) {
    if (size == nullptr) {
        return kInvalidArgument;
    }
    owner_.resize_width = static_cast<std::uint32_t>(std::max(size->right - size->left, 1));
    owner_.resize_height = static_cast<std::uint32_t>(std::max(size->bottom - size->top, 1));
    owner_.resize_requested = true;
    return kResultTrue;
}

void load_params(Vst3Plugin::Impl& s) {
    s.param_list.clear();
    s.scale.clear();
    if (s.controller == nullptr) {
        return;
    }
    const int32 count = s.controller->getParameterCount();
    for (int32 i = 0; i < count; ++i) {
        Vst::ParameterInfo info{};
        if (s.controller->getParameterInfo(i, info) != kResultOk) {
            continue;
        }
        Plugin::Param param;
        param.id = info.id;
        param.name = utf8(info.title);
        const double range = info.stepCount > 0 ? static_cast<double>(info.stepCount) : 1.0;
        param.min = 0.0;
        param.max = range;
        param.default_value = info.defaultNormalizedValue * range;
        param.stepped = info.stepCount > 0;
        param.hidden = (info.flags & (Vst::ParameterInfo::kIsReadOnly | Vst::ParameterInfo::kIsHidden)) != 0;
        s.param_list.push_back(std::move(param));
        s.scale.push_back(range);
    }
}

double scale_of(const Vst3Plugin::Impl& s, std::uint32_t id) {
    for (std::size_t i = 0; i < s.param_list.size(); ++i) {
        if (s.param_list[i].id == id) {
            return s.scale[i];
        }
    }
    return 1.0;
}

}  // namespace

Vst3Plugin::Vst3Plugin() : impl_(std::make_unique<Impl>()) {}

std::shared_ptr<Vst3Plugin> Vst3Plugin::create(const std::filesystem::path& bundle, const std::string& id,
                                               double sample_rate, std::uint32_t channels,
                                               std::string* error) {
    std::shared_ptr<Module> module = Module::open(bundle, error);
    if (!module) {
        return nullptr;
    }
    const std::vector<ClassEntry> classes = audio_classes(module->factory(), bundle);
    const ClassEntry* chosen = nullptr;
    for (const ClassEntry& entry : classes) {
        if (entry.info.id == id) {
            chosen = &entry;
        }
    }
    if (chosen == nullptr) {
        set_error(error, bundle.u8string() + " has no plugin " + id);
        return nullptr;
    }

    std::shared_ptr<Vst3Plugin> result(new Vst3Plugin());
    Impl& s = *result->impl_;
    s.module = module;
    s.info = chosen->info;
    s.sample_rate = sample_rate;
    IPluginFactory* factory = module->factory();
    if (factory->createInstance(chosen->cid, Vst::IComponent::iid, reinterpret_cast<void**>(&s.component)) != kResultOk ||
        s.component == nullptr) {
        s.component = nullptr;
        set_error(error, s.info.name + " could not be created");
        return nullptr;
    }
    if (s.component->initialize(&s.host) != kResultOk) {
        s.component->release();
        s.component = nullptr;
        set_error(error, s.info.name + " did not start");
        return nullptr;
    }
    if (s.component->queryInterface(Vst::IAudioProcessor::iid, reinterpret_cast<void**>(&s.processor)) != kResultOk) {
        s.processor = nullptr;
        set_error(error, s.info.name + " does not process audio");
        return nullptr;
    }

    // The edit controller: the same object, or a separate one that is connected to it.
    if (s.component->queryInterface(Vst::IEditController::iid, reinterpret_cast<void**>(&s.controller)) != kResultOk) {
        s.controller = nullptr;
        TUID controller_id;
        if (s.component->getControllerClassId(controller_id) == kResultOk &&
            factory->createInstance(controller_id, Vst::IEditController::iid,
                                    reinterpret_cast<void**>(&s.controller)) == kResultOk &&
            s.controller != nullptr) {
            if (s.controller->initialize(&s.host) == kResultOk) {
                s.separate_controller = true;
            } else {
                s.controller->release();
                s.controller = nullptr;
            }
        } else {
            s.controller = nullptr;
        }
    }
    if (s.controller != nullptr) {
        s.controller->setComponentHandler(&s.handler);
        if (s.separate_controller &&
            s.component->queryInterface(Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&s.component_point)) == kResultOk &&
            s.controller->queryInterface(Vst::IConnectionPoint::iid, reinterpret_cast<void**>(&s.controller_point)) == kResultOk) {
            s.component_point->connect(s.controller_point);
            s.controller_point->connect(s.component_point);
        }
        // The controller learns the state of the component.
        std::vector<char> state;
        MemoryStream out(state);
        if (s.component->getState(&out) == kResultOk) {
            MemoryStream in(state);
            s.controller->setComponentState(&in);
        }
    }

    // Buses: stereo where the plugin agrees, the main ones switched on.
    s.in_buses = s.component->getBusCount(Vst::kAudio, Vst::kInput);
    s.out_buses = s.component->getBusCount(Vst::kAudio, Vst::kOutput);
    if (s.out_buses <= 0) {
        set_error(error, s.info.name + " has no audio output");
        return nullptr;
    }
    {
        std::vector<Vst::SpeakerArrangement> ins(static_cast<std::size_t>(std::max(s.in_buses, 0)),
                                                 Vst::SpeakerArr::kStereo);
        std::vector<Vst::SpeakerArrangement> outs(static_cast<std::size_t>(s.out_buses), Vst::SpeakerArr::kStereo);
        s.processor->setBusArrangements(ins.empty() ? nullptr : ins.data(), s.in_buses, outs.data(), s.out_buses);
    }
    const auto bus_channels = [&s](Vst::BusDirection direction, std::int32_t index) {
        Vst::BusInfo bus{};
        if (s.component->getBusInfo(Vst::kAudio, direction, index, bus) != kResultOk) {
            return std::uint32_t{2};
        }
        return static_cast<std::uint32_t>(std::clamp<int32>(bus.channelCount, 0, static_cast<int32>(kMaxChannels)));
    };
    s.in_channels = s.in_buses > 0 ? bus_channels(Vst::kInput, 0) : 0;
    s.out_channels = std::max<std::uint32_t>(bus_channels(Vst::kOutput, 0), 1);
    if (s.in_buses > 0) {
        s.component->activateBus(Vst::kAudio, Vst::kInput, 0, true);
    }
    s.component->activateBus(Vst::kAudio, Vst::kOutput, 0, true);
    s.event_input = s.component->getBusCount(Vst::kEvent, Vst::kInput) > 0;
    if (s.event_input) {
        s.component->activateBus(Vst::kEvent, Vst::kInput, 0, true);
    }
    (void)channels;

    s.in_buffer.assign(static_cast<std::size_t>(kMaxFrames) * kMaxChannels, 0.0f);
    s.out_buffer.assign(static_cast<std::size_t>(kMaxFrames) * kMaxChannels, 0.0f);
    s.spare_buffer.assign(static_cast<std::size_t>(kMaxFrames) * kMaxChannels, 0.0f);
    for (std::size_t c = 0; c < kMaxChannels; ++c) {
        s.in_ptrs[c] = s.in_buffer.data() + c * kMaxFrames;
        s.out_ptrs[c] = s.out_buffer.data() + c * kMaxFrames;
        s.spare_ptrs[c] = s.spare_buffer.data() + c * kMaxFrames;
    }
    s.inputs.resize(static_cast<std::size_t>(std::max(s.in_buses, 0)));
    for (std::size_t b = 0; b < s.inputs.size(); ++b) {
        s.inputs[b] = Vst::AudioBusBuffers{};
        s.inputs[b].numChannels = static_cast<int32>(b == 0 ? s.in_channels : bus_channels(Vst::kInput, static_cast<int32>(b)));
        s.inputs[b].channelBuffers32 = b == 0 ? s.in_ptrs.data() : s.spare_ptrs.data();
    }
    s.outputs.resize(static_cast<std::size_t>(s.out_buses));
    for (std::size_t b = 0; b < s.outputs.size(); ++b) {
        s.outputs[b] = Vst::AudioBusBuffers{};
        s.outputs[b].numChannels = static_cast<int32>(b == 0 ? s.out_channels : bus_channels(Vst::kOutput, static_cast<int32>(b)));
        s.outputs[b].channelBuffers32 = b == 0 ? s.out_ptrs.data() : s.spare_ptrs.data();
    }

    Vst::ProcessSetup setup{};
    setup.processMode = Vst::kRealtime;
    setup.symbolicSampleSize = Vst::kSample32;
    setup.maxSamplesPerBlock = static_cast<int32>(kMaxFrames);
    setup.sampleRate = sample_rate;
    if (s.processor->setupProcessing(setup) != kResultOk) {
        set_error(error, s.info.name + " cannot process at this sample rate");
        return nullptr;
    }
    if (s.component->setActive(true) != kResultOk) {
        set_error(error, s.info.name + " could not get ready to process sound");
        return nullptr;
    }
    s.active = true;
    s.latency = s.processor->getLatencySamples();
    s.context.sampleRate = sample_rate;
    s.context.tempo = 120.0;
    s.context.timeSigNumerator = 4;
    s.context.timeSigDenominator = 4;
    s.context.state = Vst::ProcessContext::kTempoValid | Vst::ProcessContext::kTimeSigValid;
    load_params(s);
    return result;
}

Vst3Plugin::~Vst3Plugin() {
    Impl& s = *impl_;
    close_gui();
    if (s.processor != nullptr && s.processing) {
        s.processor->setProcessing(false);
    }
    if (s.component != nullptr && s.active) {
        s.component->setActive(false);
    }
    if (s.component_point != nullptr && s.controller_point != nullptr) {
        s.component_point->disconnect(s.controller_point);
        s.controller_point->disconnect(s.component_point);
    }
    if (s.component_point != nullptr) {
        s.component_point->release();
    }
    if (s.controller_point != nullptr) {
        s.controller_point->release();
    }
    if (s.controller != nullptr) {
        s.controller->setComponentHandler(nullptr);
        if (s.separate_controller) {
            s.controller->terminate();
        }
        s.controller->release();
    }
    if (s.processor != nullptr) {
        s.processor->release();
    }
    if (s.component != nullptr) {
        s.component->terminate();
        s.component->release();
    }
}

const PluginInfo& Vst3Plugin::info() const noexcept { return impl_->info; }

void Vst3Plugin::process(float* interleaved, std::uint32_t frames, std::uint32_t channels) noexcept {
    Impl& s = *impl_;
    if (!s.active || channels == 0 || interleaved == nullptr) {
        return;
    }
    if (!s.processing) {
        s.processor->setProcessing(true);
        s.processing = true;
    }
    s.in_changes.clear();
    s.out_changes.clear();
    s.out_events.clear();
    ParamChange change;
    std::uint64_t popped = 0;
    while (s.changes.try_pop(change)) {
        int32 index = 0;
        if (Vst::IParamValueQueue* queue = s.in_changes.addParameterData(change.id, index)) {
            int32 point = 0;
            queue->addPoint(0, change.value, point);
        }
        ++popped;
    }
    if (popped > 0) {
        s.consumed.fetch_add(popped, std::memory_order_release);
    }

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
        s.context.projectTimeSamples = s.time;
        s.context.systemTime = 0;
        Vst::ProcessData data{};
        data.processMode = Vst::kRealtime;
        data.symbolicSampleSize = Vst::kSample32;
        data.numSamples = static_cast<int32>(count);
        data.numInputs = s.in_buses;
        data.numOutputs = s.out_buses;
        data.inputs = s.inputs.empty() ? nullptr : s.inputs.data();
        data.outputs = s.outputs.data();
        data.inputParameterChanges = &s.in_changes;
        data.outputParameterChanges = &s.out_changes;
        data.inputEvents = &s.in_events;
        data.outputEvents = &s.out_events;
        data.processContext = &s.context;
        const tresult status = s.processor->process(data);
        s.in_changes.clear();  // the changes and notes belonged to the first part
        s.in_events.clear();
        s.time += count;
        if (status == kResultOk) {
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

void Vst3Plugin::add_note(std::uint32_t frame, bool on, int pitch, float velocity) noexcept {
    Impl& s = *impl_;
    if (!s.event_input || pitch > 127 || (on && pitch < 0)) {
        return;
    }
    const auto send = [&s, frame](bool note_on, int key, float strength) {
        Vst::Event event{};
        event.busIndex = 0;
        event.sampleOffset = static_cast<int32>(frame);
        event.flags = Vst::Event::kIsLive;
        if (note_on) {
            event.type = Vst::Event::kNoteOnEvent;
            event.noteOn.channel = 0;
            event.noteOn.pitch = static_cast<int16>(key);
            event.noteOn.velocity = strength;
            event.noteOn.noteId = -1;
        } else {
            event.type = Vst::Event::kNoteOffEvent;
            event.noteOff.channel = 0;
            event.noteOff.pitch = static_cast<int16>(key);
            event.noteOff.velocity = 0.0f;
            event.noteOff.noteId = -1;
        }
        s.in_events.addEvent(event);
        s.held[static_cast<std::size_t>(key)] = note_on;
    };
    if (pitch < 0) {  // every note ends: VST3 has no wildcard, so each held key gets its end
        for (int key = 0; key < 128; ++key) {
            if (s.held[static_cast<std::size_t>(key)]) {
                send(false, key, 0.0f);
            }
        }
        return;
    }
    send(on, pitch, velocity);
}

const std::vector<Plugin::Param>& Vst3Plugin::params() const noexcept { return impl_->param_list; }

double Vst3Plugin::param_value(std::uint32_t id) const {
    const Impl& s = *impl_;
    const auto found = s.requested.find(id);
    if (found != s.requested.end()) {
        return found->second.first;
    }
    if (s.controller == nullptr) {
        return 0.0;
    }
    return s.controller->getParamNormalized(id) * scale_of(s, id);
}

std::string Vst3Plugin::param_text(std::uint32_t id, double value) const {
    const Impl& s = *impl_;
    Vst::String128 text{};
    const double scale = scale_of(s, id);
    if (s.controller != nullptr &&
        s.controller->getParamStringByValue(id, std::clamp(value / scale, 0.0, 1.0), text) == kResultOk && text[0] != 0) {
        return utf8(text);
    }
    char fallback[32];
    std::snprintf(fallback, sizeof(fallback), "%.3g", value);
    return fallback;
}

bool Vst3Plugin::set_param(std::uint32_t id, double value) {
    Impl& s = *impl_;
    const double scale = scale_of(s, id);
    const double normalized = std::clamp(value / scale, 0.0, 1.0);
    if (!s.changes.try_push(ParamChange{id, normalized})) {
        return false;
    }
    if (s.controller != nullptr) {
        s.controller->setParamNormalized(id, normalized);  // the plugin's window follows
    }
    ++s.sent;
    s.requested[id] = {normalized * scale, s.sent};
    return true;
}

bool Vst3Plugin::save_state(std::vector<char>& state) const {
    const Impl& s = *impl_;
    state.clear();
    std::vector<char> component_state;
    MemoryStream component_stream(component_state);
    if (s.component->getState(&component_stream) != kResultOk) {
        return false;
    }
    std::vector<char> controller_state;
    if (s.controller != nullptr) {
        MemoryStream controller_stream(controller_state);
        if (s.controller->getState(&controller_stream) != kResultOk) {
            controller_state.clear();
        }
    }
    // [component size, 8 bytes][component state][controller state]
    const auto size = static_cast<std::uint64_t>(component_state.size());
    for (int i = 0; i < 8; ++i) {
        state.push_back(static_cast<char>((size >> (8 * i)) & 0xFF));
    }
    state.insert(state.end(), component_state.begin(), component_state.end());
    state.insert(state.end(), controller_state.begin(), controller_state.end());
    return true;
}

bool Vst3Plugin::load_state(const std::vector<char>& state) {
    Impl& s = *impl_;
    if (state.size() < 8) {
        return false;
    }
    std::uint64_t size = 0;
    for (int i = 0; i < 8; ++i) {
        size |= static_cast<std::uint64_t>(static_cast<unsigned char>(state[static_cast<std::size_t>(i)])) << (8 * i);
    }
    if (size > state.size() - 8) {
        return false;
    }
    std::vector<char> component_state(state.begin() + 8, state.begin() + 8 + static_cast<std::ptrdiff_t>(size));
    std::vector<char> controller_state(state.begin() + 8 + static_cast<std::ptrdiff_t>(size), state.end());
    MemoryStream component_stream(component_state);
    if (s.component->setState(&component_stream) != kResultOk) {
        return false;
    }
    if (s.controller != nullptr) {
        MemoryStream again(component_state);
        s.controller->setComponentState(&again);
        if (!controller_state.empty()) {
            MemoryStream controller_stream(controller_state);
            s.controller->setState(&controller_stream);
        }
    }
    s.requested.clear();
    load_params(s);
    return true;
}

bool Vst3Plugin::has_gui() const {
#ifdef _WIN32
    const Impl& s = *impl_;
    if (s.controller == nullptr) {
        return false;
    }
    if (s.view != nullptr) {
        return true;
    }
    IPlugView* view = s.controller->createView(Vst::ViewType::kEditor);
    if (view == nullptr) {
        return false;
    }
    const bool supported = view->isPlatformTypeSupported(kPlatformTypeHWND) == kResultTrue;
    view->release();
    return supported;
#else
    return false;  // X11 views need a host run loop, which is not there yet
#endif
}

bool Vst3Plugin::gui_open() const noexcept { return impl_->view != nullptr; }

bool Vst3Plugin::open_gui(void* parent, std::uint32_t& width, std::uint32_t& height, bool& resizable) {
#ifdef _WIN32
    Impl& s = *impl_;
    if (s.controller == nullptr || parent == nullptr) {
        return false;
    }
    close_gui();
    s.view = s.controller->createView(Vst::ViewType::kEditor);
    if (s.view == nullptr) {
        return false;
    }
    if (s.view->isPlatformTypeSupported(kPlatformTypeHWND) != kResultTrue) {
        close_gui();
        return false;
    }
    s.view->setFrame(&s.frame);
    if (s.view->attached(parent, kPlatformTypeHWND) != kResultOk) {
        s.view->setFrame(nullptr);
        s.view->release();
        s.view = nullptr;
        return false;
    }
    ViewRect rect{};
    if (s.view->getSize(&rect) == kResultOk && rect.right > rect.left && rect.bottom > rect.top) {
        width = static_cast<std::uint32_t>(rect.right - rect.left);
        height = static_cast<std::uint32_t>(rect.bottom - rect.top);
    } else {
        width = 640;
        height = 400;
    }
    resizable = s.view->canResize() == kResultTrue;
    return true;
#else
    (void)parent;
    (void)width;
    (void)height;
    (void)resizable;
    return false;
#endif
}

bool Vst3Plugin::resize_gui(std::uint32_t& width, std::uint32_t& height) {
    Impl& s = *impl_;
    if (s.view == nullptr) {
        return false;
    }
    ViewRect rect{0, 0, static_cast<int32>(width), static_cast<int32>(height)};
    if (s.view->canResize() == kResultTrue) {
        s.view->checkSizeConstraint(&rect);
    }
    if (s.view->onSize(&rect) != kResultOk) {
        return false;
    }
    width = static_cast<std::uint32_t>(std::max(rect.right - rect.left, 1));
    height = static_cast<std::uint32_t>(std::max(rect.bottom - rect.top, 1));
    return true;
}

void Vst3Plugin::close_gui() {
    Impl& s = *impl_;
    if (s.view == nullptr) {
        return;
    }
    s.view->removed();
    s.view->setFrame(nullptr);
    s.view->release();
    s.view = nullptr;
}

bool Vst3Plugin::take_resize_request(std::uint32_t& width, std::uint32_t& height) {
    Impl& s = *impl_;
    if (!s.resize_requested.exchange(false)) {
        return false;
    }
    width = s.resize_width;
    height = s.resize_height;
    return width > 0 && height > 0;
}

bool Vst3Plugin::take_gui_closed() { return false; }

void Vst3Plugin::idle() {
    Impl& s = *impl_;
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

std::uint32_t Vst3Plugin::latency() const noexcept { return impl_->latency; }

bool Vst3Plugin::take_latency_changed() {
    Impl& s = *impl_;
    if (!s.latency_changed.exchange(false)) {
        return false;
    }
    const std::uint32_t before = s.latency;
    s.latency = s.processor->getLatencySamples();
    return s.latency != before;
}

bool Vst3Plugin::is_instrument() const noexcept { return impl_->info.instrument || impl_->in_buses == 0; }

// ---- Both formats ---------------------------------------------------------------------------

std::shared_ptr<Plugin> create_plugin(const std::string& format, const std::filesystem::path& file,
                                      const std::string& id, double sample_rate, std::uint32_t channels,
                                      std::string* error) {
    if (format == "vst3") {
        return Vst3Plugin::create(file, id, sample_rate, channels, error);
    }
    return ClapPlugin::create(file, id, sample_rate, channels, error);
}

std::vector<PluginInfo> scan_all_plugins(const std::vector<std::filesystem::path>& extra,
                                         std::vector<std::string>* problems) {
    std::vector<std::filesystem::path> clap_folders = extra;
    for (const auto& folder : default_clap_folders()) {
        clap_folders.push_back(folder);
    }
    std::vector<std::filesystem::path> vst3_folders = extra;
    for (const auto& folder : default_vst3_folders()) {
        vst3_folders.push_back(folder);
    }
    std::vector<PluginInfo> result = scan_clap_folders(clap_folders, problems);
    std::vector<PluginInfo> vst3 = scan_vst3_folders(vst3_folders, problems);
    result.insert(result.end(), vst3.begin(), vst3.end());
    std::stable_sort(result.begin(), result.end(),
                     [](const PluginInfo& a, const PluginInfo& b) { return a.name < b.name; });
    return result;
}

}  // namespace toh::plugins
