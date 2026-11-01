#include "core/detail/work_stealing_deque.h"

#include <doctest/doctest.h>

#include <atomic>
#include <cstdint>
#include <thread>
#include <vector>

using de::core::heap_allocator;
using de::core::detail::WorkStealingDeque;

namespace {

struct Item {
    uintptr_t value = 0;
    uintptr_t check = 0; // value * 3: a torn read would break the pair
};

Item make(uintptr_t value) {
    return Item{.value = value, .check = value * 3};
}

} // namespace

TEST_CASE("the owner pops newest first, thieves steal oldest first") {
    WorkStealingDeque<Item> deque(16, heap_allocator());
    for (uintptr_t i = 1; i <= 4; ++i) {
        REQUIRE(deque.push(make(i)));
    }
    Item item;
    REQUIRE(deque.pop(item));
    CHECK(item.value == 4);
    REQUIRE(deque.steal(item));
    CHECK(item.value == 1);
    REQUIRE(deque.pop(item));
    CHECK(item.value == 3);
    REQUIRE(deque.steal(item));
    CHECK(item.value == 2);
    CHECK_FALSE(deque.pop(item));
    CHECK_FALSE(deque.steal(item));
    CHECK(deque.size_estimate() == 0);
}

TEST_CASE("push fails when full and works again after a pop") {
    WorkStealingDeque<Item> deque(4, heap_allocator());
    for (uintptr_t i = 0; i < 4; ++i) {
        REQUIRE(deque.push(make(i)));
    }
    CHECK_FALSE(deque.push(make(99)));
    Item item;
    REQUIRE(deque.pop(item));
    CHECK(deque.push(make(5)));
}

TEST_CASE("indices wrap around the ring") {
    WorkStealingDeque<Item> deque(4, heap_allocator());
    Item item;
    for (uintptr_t i = 0; i < 100; ++i) {
        REQUIRE(deque.push(make(i)));
        REQUIRE(deque.steal(item));
        CHECK(item.value == i);
    }
}

TEST_CASE("every item is taken exactly once while thieves race the owner") {
    constexpr uintptr_t kItems = 200'000;
    constexpr int kThieves = 3;
    WorkStealingDeque<Item> deque(1024, heap_allocator());
    std::vector<std::atomic<uint8_t>> taken(kItems + 1);
    std::atomic<bool> owner_done{false};
    std::atomic<int> torn{0};

    const auto take = [&](const Item& item) {
        if (item.check != item.value * 3) {
            torn.fetch_add(1);
        }
        taken[item.value].fetch_add(1);
    };

    std::vector<std::thread> thieves;
    thieves.reserve(kThieves);
    for (int t = 0; t < kThieves; ++t) {
        thieves.emplace_back([&] {
            Item item;
            while (!owner_done.load() || deque.size_estimate() > 0) {
                if (deque.steal(item)) {
                    take(item);
                }
            }
        });
    }

    // The owner pushes everything, popping now and then like a worker running its own jobs.
    Item item;
    for (uintptr_t i = 1; i <= kItems; ++i) {
        while (!deque.push(make(i))) {
            if (deque.pop(item)) {
                take(item);
            }
        }
        if (i % 7 == 0 && deque.pop(item)) {
            take(item);
        }
    }
    while (deque.pop(item)) {
        take(item);
    }
    owner_done.store(true);
    for (std::thread& thief : thieves) {
        thief.join();
    }

    CHECK(torn.load() == 0);
    int missing = 0;
    int duplicated = 0;
    for (uintptr_t i = 1; i <= kItems; ++i) {
        missing += taken[i].load() == 0 ? 1 : 0;
        duplicated += taken[i].load() > 1 ? 1 : 0;
    }
    CHECK(missing == 0);
    CHECK(duplicated == 0);
}
