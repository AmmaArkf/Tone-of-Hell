#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <type_traits>

namespace toh::core {

/// Lock-free queue for exactly one producer thread and exactly one consumer thread.
///
/// try_push and try_pop never allocate, lock or throw, so either side may run on the audio
/// thread. The capacity is fixed at compile time and must be a power of two.
template <typename T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "T must be trivially copyable");

public:
    /// Adds a value. Returns false (and drops the value) when the queue is full.
    bool try_push(const T& value) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = head + 1;
        if (next - tail_.load(std::memory_order_acquire) > Capacity) {
            return false;
        }
        slots_[head & kMask] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// Removes the oldest value. Returns false when the queue is empty.
    bool try_pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false;
        }
        out = slots_[tail & kMask];
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t kMask = Capacity - 1;

    // The two counters only ever grow; unsigned wrap-around keeps their difference correct.
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
    std::array<T, Capacity> slots_{};
};

}  // namespace toh::core
