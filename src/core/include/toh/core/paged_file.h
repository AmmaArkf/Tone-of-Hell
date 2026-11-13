#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace toh::core {

/// Opens a file with a path that may contain any characters (also on Windows).
std::FILE* open_file(const std::filesystem::path& path, const char* mode);

/// Audio kept in a file of raw interleaved 32-bit float samples and read in pages of 1 MB, so
/// that long imports and recordings do not have to fit in memory. At most kSlots pages are in
/// memory at a time.
///
/// Threads
///  - read_frame() is for the audio thread: it never blocks, allocates or touches the disk; it
///    simply reports false when the page is not in memory yet.
///  - prefetch() is for one loader thread, which brings pages into memory ahead of playback.
///  - read_frame_blocking() and read_blocking() are for any normal thread (drawing, export);
///    they read the disk through their own buffer and do not disturb the other two.
class PagedFile {
public:
    static constexpr std::size_t kPageBytes = std::size_t{1} << 20;
    static constexpr std::size_t kSlots = 6;

    /// Opens a file written as raw interleaved float samples with `channels` channels.
    static std::shared_ptr<PagedFile> open(const std::filesystem::path& path,
                                           std::uint32_t channels, std::string* error = nullptr);
    ~PagedFile();
    PagedFile(const PagedFile&) = delete;
    PagedFile& operator=(const PagedFile&) = delete;

    std::int64_t frames() const noexcept { return frames_; }
    std::uint32_t channels() const noexcept { return channels_; }
    std::int64_t frames_per_page() const noexcept { return frames_per_page_; }
    std::int64_t page_count() const noexcept {
        return (frames_ + frames_per_page_ - 1) / frames_per_page_;
    }
    const std::filesystem::path& path() const noexcept { return path_; }

    /// Audio thread: copies one frame (channels() samples) to `out` if its page is in memory.
    bool read_frame(std::int64_t frame, float* out) const noexcept;

    /// Loader thread: brings the listed pages into memory, most important first. Only the
    /// first kSlots are kept; pages that are not listed may be dropped to make room.
    void prefetch(const std::vector<std::int64_t>& pages);

    /// Any normal thread: reads one frame from the disk.
    bool read_frame_blocking(std::int64_t frame, float* out);
    /// Any normal thread: reads up to `count` frames starting at `first`. Returns the number of
    /// frames read.
    std::size_t read_blocking(std::int64_t first, std::size_t count, float* out);

    bool is_resident(std::int64_t page) const noexcept;
    /// Memory taken by pages that are loaded right now.
    std::size_t resident_bytes() const noexcept;

private:
    PagedFile() = default;

    struct Slot {
        std::atomic<std::uint32_t> version{0};  // odd while the loader rewrites the slot
        std::atomic<std::int64_t> page{-1};
        std::vector<float> data;  // allocated by the loader before the slot is first used
    };

    bool load_page(Slot& slot, std::int64_t page);
    bool read_range(std::FILE* file, std::int64_t first, std::size_t frames, float* out) const;
    bool load_blocking_page(std::int64_t page);

    std::filesystem::path path_;
    std::uint32_t channels_ = 0;
    std::int64_t frames_ = 0;
    std::int64_t frames_per_page_ = 1;
    std::array<Slot, kSlots> slots_;
    mutable std::atomic<std::size_t> hint_{0};  // the slot that answered last

    std::FILE* loader_file_ = nullptr;  // loader thread only

    std::mutex blocking_mutex_;  // guards the three members below
    std::FILE* blocking_file_ = nullptr;
    std::vector<float> blocking_page_;
    std::int64_t blocking_page_index_ = -1;
};

}  // namespace toh::core
