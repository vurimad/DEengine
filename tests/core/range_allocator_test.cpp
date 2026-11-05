#include "core/allocator.h"
#include "core/range_allocator.h"

#include <doctest/doctest.h>

#include <cstdint>

using de::core::heap_allocator;
using de::core::MemoryTag;
using de::core::RangeAllocator;

TEST_CASE("a range allocator hands out first fit, and says when nothing fits") {
    RangeAllocator ranges(heap_allocator(MemoryTag::Render), 100);
    CHECK(ranges.size() == 100);
    CHECK(ranges.free_total() == 100);
    CHECK(ranges.allocate(30) == 0);
    CHECK(ranges.allocate(30) == 30);
    CHECK(ranges.free_total() == 40);
    CHECK(ranges.largest_free() == 40);
    CHECK(ranges.allocate(41) == RangeAllocator::kNone);
    CHECK(ranges.free_total() == 40); // nothing taken
    CHECK(ranges.allocate(40) == 60); // exactly the rest
    CHECK(ranges.free_total() == 0);
    CHECK(ranges.largest_free() == 0);
    CHECK(ranges.allocate(1) == RangeAllocator::kNone);
}

TEST_CASE("freed ranges are reused, first fit from the start, and merge with their neighbours") {
    RangeAllocator ranges(heap_allocator(MemoryTag::Render), 100);
    const uint64_t a = ranges.allocate(20);
    const uint64_t b = ranges.allocate(20);
    const uint64_t c = ranges.allocate(20);
    const uint64_t d = ranges.allocate(20);
    CHECK(ranges.free_total() == 20);

    // Apart, they're pieces: no piece holds what they'd hold together.
    ranges.free(a, 20);
    ranges.free(c, 20);
    CHECK(ranges.free_total() == 60);
    CHECK(ranges.largest_free() == 20);
    CHECK(ranges.allocate(21) == RangeAllocator::kNone);

    // The first that fits, not the best.
    CHECK(ranges.allocate(10) == a);
    CHECK(ranges.allocate(15) == c);
    CHECK(ranges.largest_free() == 20);
    ranges.free(a, 10);
    ranges.free(c, 15);

    // Between two free ranges: all three become one.
    ranges.free(b, 20);
    CHECK(ranges.largest_free() == 60);
    CHECK(ranges.allocate(60) == a);
    ranges.free(a, 60);

    // Up against the end of the range after it, and the start of the one before it.
    ranges.free(d, 20);
    CHECK(ranges.free_total() == 100);
    CHECK(ranges.largest_free() == 100);
    CHECK(ranges.allocate(100) == 0);
}

TEST_CASE("freed in any order, a range allocator ends up whole") {
    RangeAllocator ranges(heap_allocator(MemoryTag::Render), 64);
    uint64_t offsets[8] = {};
    for (uint64_t& offset : offsets) {
        offset = ranges.allocate(8);
    }
    for (const size_t i : {5U, 1U, 7U, 3U, 0U, 6U, 2U, 4U}) {
        ranges.free(offsets[i], 8);
    }
    CHECK(ranges.free_total() == 64);
    CHECK(ranges.largest_free() == 64);
}

TEST_CASE("a range allocator of nothing has nothing to give") {
    RangeAllocator ranges(heap_allocator(MemoryTag::Render), 0);
    CHECK(ranges.free_total() == 0);
    CHECK(ranges.largest_free() == 0);
    CHECK(ranges.allocate(1) == RangeAllocator::kNone);
}
