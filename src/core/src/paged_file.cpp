#include "toh/core/paged_file.h"

#include <algorithm>

namespace toh::core {
namespace {

void set_error(std::string* error, const std::string& message) {
    if (error != nullptr) {
        *error = message;
    }
}

bool seek_to(std::FILE* file, std::int64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, offset, SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

}  // namespace

std::FILE* open_file(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
    std::wstring wide_mode;
    for (const char* c = mode; *c != '\0'; ++c) {
        wide_mode += static_cast<wchar_t>(*c);
    }
    return _wfopen(path.c_str(), wide_mode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

std::shared_ptr<PagedFile> PagedFile::open(const std::filesystem::path& path,
                                           std::uint32_t channels, std::string* error) {
    if (channels == 0) {
        set_error(error, "invalid channel count");
        return nullptr;
    }
    std::error_code code;
    const std::uintmax_t bytes = std::filesystem::file_size(path, code);
    if (code) {
        set_error(error, "cannot open the cache file: " + code.message());
        return nullptr;
    }

    std::shared_ptr<PagedFile> file(new PagedFile());
    file->path_ = path;
    file->channels_ = channels;
    const std::uintmax_t frame_bytes = sizeof(float) * channels;
    file->frames_ = static_cast<std::int64_t>(bytes / frame_bytes);
    file->frames_per_page_ =
        std::max<std::int64_t>(1, static_cast<std::int64_t>(kPageBytes / frame_bytes));
    return file;
}

PagedFile::~PagedFile() {
    if (loader_file_ != nullptr) {
        std::fclose(loader_file_);
    }
    if (blocking_file_ != nullptr) {
        std::fclose(blocking_file_);
    }
}

// A sequence lock: the frame is copied, then the slot's version is checked again. If the loader
// rewrote the slot meanwhile the copy is thrown away, so a torn page is never played.
bool PagedFile::read_frame(std::int64_t frame, float* out) const noexcept {
    if (frame < 0 || frame >= frames_) {
        return false;
    }
    const std::int64_t page = frame / frames_per_page_;
    const auto offset = static_cast<std::size_t>(frame - page * frames_per_page_) * channels_;
    const std::size_t start = hint_.load(std::memory_order_relaxed);

    for (std::size_t n = 0; n < kSlots; ++n) {
        const std::size_t index = (start + n) % kSlots;
        const Slot& slot = slots_[index];
        const std::uint32_t before = slot.version.load(std::memory_order_acquire);
        if ((before & 1u) != 0 || slot.page.load(std::memory_order_relaxed) != page) {
            continue;
        }
        const float* source = slot.data.data() + offset;
        for (std::size_t c = 0; c < channels_; ++c) {
            out[c] = source[c];
        }
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.version.load(std::memory_order_relaxed) == before) {
            if (n != 0) {
                hint_.store(index, std::memory_order_relaxed);
            }
            return true;
        }
    }
    return false;
}

bool PagedFile::read_range(std::FILE* file, std::int64_t first, std::size_t frames,
                           float* out) const {
    const auto offset = first * static_cast<std::int64_t>(sizeof(float) * channels_);
    const std::size_t samples = frames * channels_;
    return seek_to(file, offset) && std::fread(out, sizeof(float), samples, file) == samples;
}

bool PagedFile::load_page(Slot& slot, std::int64_t page) {
    if (loader_file_ == nullptr) {
        loader_file_ = open_file(path_, "rb");
        if (loader_file_ == nullptr) {
            return false;
        }
    }
    const std::int64_t first = page * frames_per_page_;
    const auto frames = static_cast<std::size_t>(std::min(frames_per_page_, frames_ - first));

    slot.version.fetch_add(1, std::memory_order_relaxed);  // odd: readers stay away
    std::atomic_thread_fence(std::memory_order_release);
    slot.page.store(page, std::memory_order_relaxed);
    if (slot.data.empty()) {
        slot.data.assign(static_cast<std::size_t>(frames_per_page_) * channels_, 0.0f);
    }
    const bool ok = read_range(loader_file_, first, frames, slot.data.data());
    if (!ok) {
        slot.page.store(-1, std::memory_order_relaxed);
    }
    slot.version.fetch_add(1, std::memory_order_release);  // even: the page is published
    return ok;
}

void PagedFile::prefetch(const std::vector<std::int64_t>& pages) {
    // The pages to keep: valid, without repeats, at most one per slot.
    std::vector<std::int64_t> wanted;
    const std::int64_t count = page_count();
    for (const std::int64_t page : pages) {
        if (page >= 0 && page < count &&
            std::find(wanted.begin(), wanted.end(), page) == wanted.end()) {
            wanted.push_back(page);
            if (wanted.size() == kSlots) {
                break;
            }
        }
    }

    for (const std::int64_t page : wanted) {
        if (is_resident(page)) {
            continue;
        }
        // Use an empty slot, otherwise one whose page is no longer wanted.
        Slot* target = nullptr;
        for (Slot& slot : slots_) {
            if (slot.page.load(std::memory_order_relaxed) < 0) {
                target = &slot;
                break;
            }
        }
        if (target == nullptr) {
            for (Slot& slot : slots_) {
                const std::int64_t held = slot.page.load(std::memory_order_relaxed);
                if (std::find(wanted.begin(), wanted.end(), held) == wanted.end()) {
                    target = &slot;
                    break;
                }
            }
        }
        if (target != nullptr) {
            load_page(*target, page);
        }
    }
}

// The caller holds blocking_mutex_.
bool PagedFile::load_blocking_page(std::int64_t page) {
    if (page == blocking_page_index_) {
        return true;
    }
    if (blocking_file_ == nullptr) {
        blocking_file_ = open_file(path_, "rb");
        if (blocking_file_ == nullptr) {
            return false;
        }
    }
    blocking_page_.resize(static_cast<std::size_t>(frames_per_page_) * channels_);
    const std::int64_t first = page * frames_per_page_;
    const auto frames = static_cast<std::size_t>(std::min(frames_per_page_, frames_ - first));
    if (!read_range(blocking_file_, first, frames, blocking_page_.data())) {
        blocking_page_index_ = -1;
        return false;
    }
    blocking_page_index_ = page;
    return true;
}

bool PagedFile::read_frame_blocking(std::int64_t frame, float* out) {
    return read_blocking(frame, 1, out) == 1;
}

std::size_t PagedFile::read_blocking(std::int64_t first, std::size_t count, float* out) {
    if (first < 0) {
        return 0;
    }
    const std::lock_guard<std::mutex> lock(blocking_mutex_);
    std::size_t done = 0;
    while (done < count) {
        const std::int64_t frame = first + static_cast<std::int64_t>(done);
        if (frame >= frames_) {
            break;
        }
        const std::int64_t page = frame / frames_per_page_;
        if (!load_blocking_page(page)) {
            break;
        }
        // Copy as much of this page as is wanted in one go.
        const std::int64_t page_end = std::min(frames_, (page + 1) * frames_per_page_);
        const auto n = static_cast<std::size_t>(
            std::min<std::int64_t>(page_end - frame, static_cast<std::int64_t>(count - done)));
        const auto offset = static_cast<std::size_t>(frame - page * frames_per_page_) * channels_;
        std::copy_n(blocking_page_.data() + offset, n * channels_, out + done * channels_);
        done += n;
    }
    return done;
}

bool PagedFile::is_resident(std::int64_t page) const noexcept {
    for (const Slot& slot : slots_) {
        if (slot.page.load(std::memory_order_relaxed) == page) {
            return true;
        }
    }
    return false;
}

std::size_t PagedFile::resident_bytes() const noexcept {
    std::size_t pages = 0;
    for (const Slot& slot : slots_) {
        if (slot.page.load(std::memory_order_relaxed) >= 0) {
            ++pages;
        }
    }
    return pages * kPageBytes;
}

}  // namespace toh::core
