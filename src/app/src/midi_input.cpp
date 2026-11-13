#include "toh/app/midi_input.h"

#include <atomic>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#endif

namespace toh::app {

struct MidiInput::Impl {
    std::mutex mutex;
    std::vector<MidiEvent> queue;
    std::vector<std::string> names;
#ifdef _WIN32
    std::vector<HMIDIIN> handles;
#else
    std::vector<int> files;
    std::atomic<bool> stop{false};
    std::thread reader;
#endif

    // One complete channel message: note-ons and note-offs go to the queue.
    void message(unsigned status, unsigned data1, unsigned data2) {
        const unsigned type = status & 0xF0u;
        if (type != 0x80u && type != 0x90u) {
            return;
        }
        MidiEvent event;
        event.pitch = static_cast<int>(data1 & 0x7Fu);
        event.velocity = static_cast<float>(data2 & 0x7Fu) / 127.0f;
        event.on = type == 0x90u && (data2 & 0x7Fu) != 0;  // a note-on of velocity 0 ends a note
        const std::lock_guard<std::mutex> lock(mutex);
        if (queue.size() < 4096) {
            queue.push_back(event);
        }
    }
};

#ifdef _WIN32

namespace {

void CALLBACK on_midi(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
    if (message != MIM_DATA) {
        return;
    }
    auto* impl = reinterpret_cast<MidiInput::Impl*>(instance);
    impl->message(static_cast<unsigned>(param1 & 0xFFu), static_cast<unsigned>((param1 >> 8) & 0xFFu),
                  static_cast<unsigned>((param1 >> 16) & 0xFFu));
}

std::string to_utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return std::string();
    }
    std::string result(static_cast<std::size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), size, nullptr, nullptr);
    return result;
}

}  // namespace

void MidiInput::open_all() {
    close_all();
    const UINT count = midiInGetNumDevs();
    for (UINT i = 0; i < count; ++i) {
        MIDIINCAPSW caps{};
        if (midiInGetDevCapsW(i, &caps, sizeof(caps)) != MMSYSERR_NOERROR) {
            continue;
        }
        HMIDIIN handle = nullptr;
        if (midiInOpen(&handle, i, reinterpret_cast<DWORD_PTR>(&on_midi),
                       reinterpret_cast<DWORD_PTR>(impl_.get()), CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
            continue;
        }
        midiInStart(handle);
        impl_->handles.push_back(handle);
        impl_->names.push_back(to_utf8(caps.szPname));
    }
}

void MidiInput::close_all() {
    for (HMIDIIN handle : impl_->handles) {
        midiInStop(handle);
        midiInReset(handle);
        midiInClose(handle);
    }
    impl_->handles.clear();
    impl_->names.clear();
}

#else

void MidiInput::open_all() {
    close_all();
    if (DIR* dir = opendir("/dev/snd")) {
        while (const dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name.rfind("midiC", 0) != 0) {
                continue;
            }
            const int file = open(("/dev/snd/" + name).c_str(), O_RDONLY | O_NONBLOCK);
            if (file >= 0) {
                impl_->files.push_back(file);
                impl_->names.push_back(name);
            }
        }
        closedir(dir);
    }
    if (impl_->files.empty()) {
        return;
    }
    impl_->stop = false;
    Impl* impl = impl_.get();
    impl_->reader = std::thread([impl] {
        // Raw MIDI bytes; every device keeps its own running status.
        std::vector<unsigned> status(impl->files.size(), 0);
        std::vector<std::vector<unsigned>> data(impl->files.size());
        while (!impl->stop) {
            bool any = false;
            for (std::size_t d = 0; d < impl->files.size(); ++d) {
                unsigned char buffer[256];
                const ssize_t got = read(impl->files[d], buffer, sizeof(buffer));
                for (ssize_t i = 0; i < got; ++i) {
                    const unsigned b = buffer[i];
                    any = true;
                    if (b >= 0xF8u) {
                        continue;  // real-time messages (clock, active sensing)
                    }
                    if (b >= 0x80u) {
                        status[d] = b < 0xF0u ? b : 0u;
                        data[d].clear();
                        continue;
                    }
                    if (status[d] == 0) {
                        continue;
                    }
                    data[d].push_back(b);
                    const unsigned type = status[d] & 0xF0u;
                    const std::size_t needed = (type == 0xC0u || type == 0xD0u) ? 1 : 2;
                    if (data[d].size() == needed) {
                        impl->message(status[d], data[d][0], needed > 1 ? data[d][1] : 0u);
                        data[d].clear();
                    }
                }
            }
            if (!any) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    });
}

void MidiInput::close_all() {
    impl_->stop = true;
    if (impl_->reader.joinable()) {
        impl_->reader.join();
    }
    for (const int file : impl_->files) {
        close(file);
    }
    impl_->files.clear();
    impl_->names.clear();
}

#endif

MidiInput::MidiInput() : impl_(std::make_unique<Impl>()) {}

MidiInput::~MidiInput() { close_all(); }

const std::vector<std::string>& MidiInput::devices() const noexcept { return impl_->names; }

void MidiInput::take(std::vector<MidiEvent>& events) {
    events.clear();
    const std::lock_guard<std::mutex> lock(impl_->mutex);
    events.swap(impl_->queue);
}

}  // namespace toh::app
