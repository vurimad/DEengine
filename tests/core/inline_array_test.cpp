#include "core/inline_array.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <numeric>
#include <span>
#include <type_traits>

using de::core::InlineArray;
using de::test::CountingAllocator;
using de::test::MoveOnly;
using de::test::Tracked;

static_assert(!std::is_copy_constructible_v<InlineArray<int, 4>>, "copies must be explicit");
static_assert(std::is_nothrow_move_constructible_v<InlineArray<int, 4>>);

namespace {

int sum(std::span<const int> values) {
    return std::accumulate(values.begin(), values.end(), 0);
}

} // namespace

TEST_CASE("stays inline up to N elements and never touches the allocator") {
    CountingAllocator counting;
    InlineArray<int, 4> numbers(counting);
    for (int i = 0; i < 4; ++i) {
        numbers.push(i);
    }
    CHECK(numbers.is_inline());
    CHECK(numbers.capacity() == 4);
    CHECK(counting.allocations == 0);
    CHECK(sum(numbers) == 6);
}

TEST_CASE("spills to the allocator beyond N and keeps every element") {
    CountingAllocator counting;
    {
        InlineArray<int, 4> numbers(counting);
        for (int i = 0; i < 100; ++i) {
            numbers.push(i);
        }
        CHECK_FALSE(numbers.is_inline());
        CHECK(counting.allocations > 0);
        for (int i = 0; i < 100; ++i) {
            CHECK(numbers[static_cast<size_t>(i)] == i);
        }
    }
    CHECK(counting.allocations == counting.deallocations);
    CHECK(counting.live_bytes == 0);
}

TEST_CASE("pushing one of its own elements while growing is safe") {
    InlineArray<Tracked, 2> items;
    items.emplace(10);
    items.emplace(20);
    REQUIRE(items.size() == items.capacity());
    items.push(items[0]); // grows: the argument lives in the old storage
    REQUIRE(items.size() == 3);
    CHECK(items[2].value == 10);
    CHECK(items[0].value == 10);
}

TEST_CASE("moving an inline array relocates its elements") {
    const int live_before = Tracked::live;
    {
        InlineArray<Tracked, 4> source;
        source.emplace(1);
        source.emplace(2);

        const InlineArray<Tracked, 4> target(std::move(source));
        CHECK(source.empty()); // NOLINT(bugprone-use-after-move): moved-from state is specified
        CHECK(source.is_inline());
        REQUIRE(target.size() == 2);
        CHECK(target[1].value == 2);
        CHECK(Tracked::live - live_before == 2);
    }
    CHECK(Tracked::live == live_before);
}

TEST_CASE("moving a spilled array steals the heap block") {
    CountingAllocator counting;
    {
        InlineArray<int, 2> source(counting);
        for (int i = 0; i < 10; ++i) {
            source.push(i);
        }
        const int allocations = counting.allocations;
        const int* block = source.data();

        const InlineArray<int, 2> target(std::move(source));
        CHECK(target.data() == block);
        CHECK(counting.allocations == allocations);
        CHECK(source.empty()); // NOLINT(bugprone-use-after-move): moved-from state is specified
        CHECK(source.is_inline());
        CHECK(target.size() == 10);
    }
    CHECK(counting.live_bytes == 0);
}

TEST_CASE("move assignment covers inline and spilled on both sides") {
    CountingAllocator counting;
    const int live_before = Tracked::live;
    {
        InlineArray<Tracked, 2> small(counting);
        small.emplace(1);

        InlineArray<Tracked, 2> big(counting);
        for (int i = 0; i < 5; ++i) {
            big.emplace(i);
        }

        small = std::move(big); // spilled into inline
        REQUIRE(small.size() == 5);
        CHECK(small[4].value == 4);

        InlineArray<Tracked, 2> other(counting);
        other.emplace(9);
        small = std::move(other); // inline into spilled: frees the old block
        REQUIRE(small.size() == 1);
        CHECK(small[0].value == 9);
        CHECK(small.is_inline());
    }
    CHECK(Tracked::live == live_before);
    CHECK(counting.live_bytes == 0);
}

TEST_CASE("remove, resize and clear keep the element count exact") {
    const int live_before = Tracked::live;
    {
        InlineArray<Tracked, 4> items;
        for (int i = 0; i < 8; ++i) {
            items.emplace(i);
        }
        items.remove_swap(0);
        CHECK(items[0].value == 7);
        items.remove_ordered(1);
        CHECK(items[1].value == 2);
        CHECK(Tracked::live - live_before == 6);

        items.resize(10);
        CHECK(Tracked::live - live_before == 10);
        items.resize(3);
        CHECK(Tracked::live - live_before == 3);

        const size_t capacity = items.capacity();
        items.clear();
        CHECK(items.empty());
        CHECK(items.capacity() == capacity);
        CHECK(Tracked::live == live_before);
    }
    CHECK(Tracked::live == live_before);
}

TEST_CASE("holds move-only types") {
    InlineArray<MoveOnly, 1> items;
    items.emplace(1);
    items.emplace(2); // spills
    items.remove_ordered(0);
    REQUIRE(items.size() == 1);
    CHECK(items[0].value == 2);
}
