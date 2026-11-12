#include <cstdint>
#include <thread>

#include "doctest.h"
#include "toh/core/spsc_queue.h"

using toh::core::SpscQueue;

TEST_CASE("SpscQueue keeps order and reports full and empty") {
    SpscQueue<int, 4> queue;
    int value = 0;
    CHECK_FALSE(queue.try_pop(value));

    for (int i = 0; i < 4; ++i) {
        CHECK(queue.try_push(i));
    }
    CHECK_FALSE(queue.try_push(99));  // full

    for (int i = 0; i < 4; ++i) {
        REQUIRE(queue.try_pop(value));
        CHECK(value == i);
    }
    CHECK_FALSE(queue.try_pop(value));

    // Still works after the indices wrap around the buffer.
    for (int round = 0; round < 10; ++round) {
        CHECK(queue.try_push(round));
        CHECK(queue.try_push(round + 100));
        REQUIRE(queue.try_pop(value));
        CHECK(value == round);
        REQUIRE(queue.try_pop(value));
        CHECK(value == round + 100);
    }
}

TEST_CASE("SpscQueue passes every value between two threads") {
    constexpr std::uint32_t kCount = 200000;
    SpscQueue<std::uint32_t, 64> queue;

    std::thread producer([&queue] {
        for (std::uint32_t i = 0; i < kCount;) {
            if (queue.try_push(i)) {
                ++i;
            } else {
                std::this_thread::yield();
            }
        }
    });

    std::uint32_t expected = 0;
    bool in_order = true;
    while (expected < kCount) {
        std::uint32_t value = 0;
        if (queue.try_pop(value)) {
            in_order = in_order && value == expected;
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    CHECK(in_order);
}
