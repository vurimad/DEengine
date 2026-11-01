#include "core/hash_map.h"
#include "core/slot_map.h"
#include "test_support.h"

#include <doctest/doctest.h>

using de::core::HashMap;
using de::core::SlotMap;
using de::test::CountingAllocator;
using de::test::Tracked;

TEST_CASE("insert and get") {
    SlotMap<int> map;
    const auto a = map.insert(10);
    const auto b = map.emplace(20);
    REQUIRE(map.size() == 2);
    REQUIRE(map.get(a) != nullptr);
    CHECK(*map.get(a) == 10);
    CHECK(*map.get(b) == 20);
    CHECK(a.is_valid());
}

TEST_CASE("default and foreign handles never resolve") {
    SlotMap<int> map;
    map.insert(1);
    CHECK(map.get(SlotMap<int>::Handle{}) == nullptr);
    CHECK(map.get(SlotMap<int>::Handle{.index = 99, .generation = 1}) == nullptr);
    CHECK(map.get(SlotMap<int>::Handle{.index = 0, .generation = 2}) == nullptr);
}

TEST_CASE("a removed handle stays dead after its slot is reused") {
    SlotMap<int> map;
    const auto old_handle = map.insert(1);
    CHECK(map.remove(old_handle));
    CHECK_FALSE(map.remove(old_handle));
    CHECK(map.get(old_handle) == nullptr);

    const auto new_handle = map.insert(2);
    CHECK(new_handle.index == old_handle.index); // slot reused
    CHECK(new_handle.generation != old_handle.generation);
    CHECK(map.get(old_handle) == nullptr);
    REQUIRE(map.get(new_handle) != nullptr);
    CHECK(*map.get(new_handle) == 2);
}

TEST_CASE("removal keeps every other handle pointing at its own value") {
    SlotMap<int> map;
    const auto a = map.insert(1);
    const auto b = map.insert(2);
    const auto c = map.insert(3);
    const auto d = map.insert(4);

    map.remove(b); // d's value moves into b's place
    CHECK(*map.get(a) == 1);
    CHECK(*map.get(c) == 3);
    CHECK(*map.get(d) == 4);

    map.remove(a);
    CHECK(*map.get(c) == 3);
    CHECK(*map.get(d) == 4);
    CHECK(map.size() == 2);
}

TEST_CASE("values are dense and handle_at maps back to each value") {
    SlotMap<int> map;
    for (int i = 0; i < 10; ++i) {
        map.insert(i);
    }
    map.remove(map.handle_at(3));

    int count = 0;
    for (size_t i = 0; i < map.values().size(); ++i) {
        const auto handle = map.handle_at(i);
        REQUIRE(map.get(handle) != nullptr);
        CHECK(map.get(handle) == &map.values()[i]);
        ++count;
    }
    CHECK(count == 9);

    int sum = 0;
    for (const int value : map) {
        sum += value;
    }
    CHECK(sum == 45 - 3);
}

TEST_CASE("clear invalidates every handle and reuses slots") {
    SlotMap<int> map;
    const auto a = map.insert(1);
    const auto b = map.insert(2);
    map.remove(a);
    map.clear();
    CHECK(map.empty());
    CHECK(map.get(a) == nullptr);
    CHECK(map.get(b) == nullptr);

    const auto c = map.insert(3);
    CHECK(c.index == 0);
    CHECK(*map.get(c) == 3);
    CHECK(map.get(b) == nullptr);
}

TEST_CASE("constructs and destroys elements exactly once") {
    const int live_before = Tracked::live;
    CountingAllocator counting;
    {
        SlotMap<Tracked> map(counting);
        SlotMap<Tracked>::Handle handles[20];
        for (int i = 0; i < 20; ++i) {
            handles[i] = map.emplace(i);
        }
        for (int i = 0; i < 20; i += 3) {
            map.remove(handles[i]);
        }
        CHECK(Tracked::live - live_before == 13);
        for (int i = 1; i < 20; i += 3) {
            REQUIRE(map.get(handles[i]) != nullptr);
            CHECK(map.get(handles[i])->value == i);
        }
    }
    CHECK(Tracked::live == live_before);
    CHECK(counting.live_bytes == 0);
}

TEST_CASE("handles work as hash map keys") {
    SlotMap<int> map;
    HashMap<SlotMap<int>::Handle, const char*> names;
    const auto a = map.insert(1);
    const auto b = map.insert(2);
    names.set(a, "a");
    names.set(b, "b");
    REQUIRE(names.find(b) != nullptr);
    CHECK(**names.find(b) == 'b');
}
