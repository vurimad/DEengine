#include "core/array.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <numeric>
#include <span>
#include <type_traits>

using de::core::Array;
using de::test::CountingAllocator;
using de::test::MoveOnly;
using de::test::Tracked;

static_assert(!std::is_copy_constructible_v<Array<int>>, "copies must be explicit");
static_assert(std::is_nothrow_move_constructible_v<Array<int>>);

namespace {

int sum(std::span<const int> values) {
    return std::accumulate(values.begin(), values.end(), 0);
}

} // namespace

TEST_CASE("push, emplace and indexing") {
    Array<int> numbers;
    CHECK(numbers.empty());
    numbers.push(1);
    numbers.emplace(2);
    const int three = 3;
    numbers.push(three);

    REQUIRE(numbers.size() == 3);
    CHECK(numbers[0] == 1);
    CHECK(numbers[2] == 3);
    CHECK(numbers.front() == 1);
    CHECK(numbers.back() == 3);
}

TEST_CASE("growth keeps every element") {
    Array<int> numbers;
    for (int i = 0; i < 1000; ++i) {
        numbers.push(i);
    }
    REQUIRE(numbers.size() == 1000);
    for (int i = 0; i < 1000; ++i) {
        CHECK(numbers[static_cast<size_t>(i)] == i);
    }
}

TEST_CASE("converts to std::span without copying") {
    Array<int> numbers;
    numbers.push(4);
    numbers.push(5);
    CHECK(sum(numbers) == 9);
    CHECK(numbers.span().data() == numbers.data());
}

TEST_CASE("remove_swap is O(1) and fills the hole with the last element") {
    Array<int> numbers;
    for (int i = 0; i < 5; ++i) {
        numbers.push(i);
    }
    numbers.remove_swap(1);
    REQUIRE(numbers.size() == 4);
    CHECK(numbers[1] == 4);
    numbers.remove_swap(3); // last element: plain pop
    CHECK(numbers.size() == 3);
}

TEST_CASE("remove_ordered keeps order") {
    Array<int> numbers;
    for (int i = 0; i < 5; ++i) {
        numbers.push(i);
    }
    numbers.remove_ordered(1);
    REQUIRE(numbers.size() == 4);
    CHECK(numbers[0] == 0);
    CHECK(numbers[1] == 2);
    CHECK(numbers[3] == 4);
}

TEST_CASE("resize value-initializes, clear keeps capacity") {
    Array<int> numbers;
    numbers.resize(8);
    CHECK(numbers.size() == 8);
    CHECK(numbers[7] == 0);
    numbers.resize(10, 42);
    CHECK(numbers[9] == 42);

    numbers.resize_uninit(16);
    CHECK(numbers.size() == 16);

    const size_t capacity = numbers.capacity();
    numbers.clear();
    CHECK(numbers.empty());
    CHECK(numbers.capacity() == capacity);
}

TEST_CASE("assign is the explicit copy") {
    Array<int> source;
    source.push(7);
    source.push(8);
    Array<int> copy;
    copy.assign(source);
    REQUIRE(copy.size() == 2);
    CHECK(copy[1] == 8);
    CHECK(copy.data() != source.data());
}

TEST_CASE("allocates through the given allocator and frees everything") {
    CountingAllocator counting;
    {
        Array<int> numbers(counting);
        CHECK(&numbers.allocator() == &counting);
        for (int i = 0; i < 100; ++i) {
            numbers.push(i);
        }
        CHECK(counting.allocations > 0);

        const Array<int> moved(std::move(numbers));
        CHECK(&moved.allocator() == &counting);
        CHECK(moved.size() == 100);
    }
    CHECK(counting.allocations == counting.deallocations);
    CHECK(counting.live_bytes == 0);
}

TEST_CASE("constructs and destroys elements exactly once") {
    const int live_before = Tracked::live;
    {
        Array<Tracked> items;
        for (int i = 0; i < 50; ++i) {
            items.emplace(i);
        }
        items.remove_swap(3);
        items.remove_ordered(10);
        items.pop();
        CHECK(Tracked::live - live_before == 47);
    }
    CHECK(Tracked::live == live_before);
}

TEST_CASE("holds move-only types") {
    Array<MoveOnly> items;
    items.emplace(1);
    items.push(MoveOnly(2));
    items.remove_swap(0);
    REQUIRE(items.size() == 1);
    CHECK(items[0].value == 2);
}
