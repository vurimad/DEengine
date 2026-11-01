#include "core/hash_map.h"
#include "core/string_id.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <type_traits>

using de::core::HashMap;
using de::core::StringId;
using de::test::CountingAllocator;
using de::test::MoveOnly;
using namespace de::core::literals;

static_assert(!std::is_copy_constructible_v<HashMap<int, int>>, "copies must be explicit");

TEST_CASE("set, find, contains and remove") {
    HashMap<int, float> map;
    CHECK(map.empty());
    CHECK(map.find(1) == nullptr);

    map.set(1, 1.5f);
    map.set(2, 2.5f);
    REQUIRE(map.size() == 2);
    REQUIRE(map.find(1) != nullptr);
    CHECK(*map.find(1) == 1.5f);
    CHECK(map.contains(2));

    map.set(1, 9.0f); // overwrites
    CHECK(map.size() == 2);
    CHECK(*map.find(1) == 9.0f);

    CHECK(map.remove(1));
    CHECK_FALSE(map.remove(1));
    CHECK_FALSE(map.contains(1));
    CHECK(map.size() == 1);
}

TEST_CASE("find_or_insert default-constructs missing values") {
    HashMap<int, int> counts;
    counts.find_or_insert(5) += 1;
    counts.find_or_insert(5) += 1;
    counts.find_or_insert(6) += 1;
    CHECK(*counts.find(5) == 2);
    CHECK(*counts.find(6) == 1);
}

TEST_CASE("iteration visits every entry once") {
    HashMap<int, int> map;
    for (int i = 0; i < 100; ++i) {
        map.set(i, i * 2);
    }

    int key_sum = 0;
    int value_sum = 0;
    for (auto [key, value] : map) {
        key_sum += key;
        value_sum += value;
        value += 1; // values are mutable through the entry
    }
    CHECK(key_sum == 4950);
    CHECK(value_sum == 9900);
    CHECK(*map.find(10) == 21);

    const HashMap<int, int>& view = map;
    int count = 0;
    for (const auto entry : view) {
        CHECK(entry.value == entry.key * 2 + 1);
        ++count;
    }
    CHECK(count == 100);
}

TEST_CASE("survives many inserts and removals") {
    HashMap<uint64_t, uint64_t> map;
    map.reserve(64);
    for (uint64_t i = 0; i < 5000; ++i) {
        map.set(i * 4096, i); // aligned keys: the hash must spread them
    }
    for (uint64_t i = 0; i < 5000; i += 2) {
        CHECK(map.remove(i * 4096));
    }
    CHECK(map.size() == 2500);
    CHECK(map.find(uint64_t{4096} * 2) == nullptr);
    REQUIRE(map.find(uint64_t{4096} * 3) != nullptr);
    CHECK(*map.find(uint64_t{4096} * 3) == 3);
}

TEST_CASE("StringId keys") {
    HashMap<StringId, int> damage;
    damage.set("longsword_r1_1"_sid, 110);
    damage.set("longsword_r1_2"_sid, 120);
    REQUIRE(damage.find("longsword_r1_1"_sid) != nullptr);
    CHECK(*damage.find("longsword_r1_1"_sid) == 110);
    CHECK(damage.find("greatsword_r1_1"_sid) == nullptr);
}

TEST_CASE("move-only values") {
    HashMap<int, MoveOnly> map;
    map.set(1, MoveOnly(10));
    REQUIRE(map.find(1) != nullptr);
    CHECK(map.find(1)->value == 10);
}

TEST_CASE("allocates through the given allocator and frees everything") {
    CountingAllocator counting;
    {
        HashMap<int, int> map(counting);
        for (int i = 0; i < 100; ++i) {
            map.set(i, i);
        }
        CHECK(counting.allocations > 0);
        const HashMap<int, int> moved(std::move(map));
        CHECK(moved.size() == 100);
        CHECK(&moved.allocator() == &counting);
    }
    CHECK(counting.live_bytes == 0);
}
