#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace toh::core {

/// Lock-free ring buffer of float samples for exactly one writer thread and one reader thread.
///
/// The memory is allocated once in the constructor; write and read never allocate, lock or
/// throw, so either side may run on the audio thread.
class SpscRing {
public:
    /// `capacity` is rounded up to a power of two (at least 2).
    explicit SpscRing(std::size_t capacity) {
        std::size_t size = 2;
        while (size < capacity) {
            size *= 2;
        }
        buffer_.assign(size, 0.0f);
        mask_ = size - 1;
    }

    /// Writes all `count` samples, or nothing when there is not enough room. Returns the number
    /// of samples written (count or 0).
    std::size_t write(const float* source, std::size_t count) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t used = head - tail_.load(std::memory_order_acquire);
        if (count > buffer_.size() - used) {
            return 0;
        }
        for (std::size_t i = 0; i < count; ++i) {
            buffer_[(head + i) & mask_] = source[i];
        }
        head_.store(head + count, std::memory_order_release);
        return count;
    }

    /// Reads up to `count` samples. Returns the number of samples read.
    std::size_t read(float* destination, std::size_t count) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t available = head_.load(std::memory_order_acquire) - tail;
        const std::size_t amount = std::min(count, available);
        for (std::size_t i = 0; i < amount; ++i) {
            destination[i] = buffer_[(tail + i) & mask_];
        }
        tail_.store(tail + amount, std::memory_order_release);
        return amount;
    }

    std::size_t capacity() const noexcept { return buffer_.size(); }

private:
    std::vector<float> buffer_;
    std::size_t mask_ = 1;
    // The two counters only ever grow; unsigned wrap-around keeps their difference correct.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

}  // namespace toh::core
