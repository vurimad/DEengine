#include "core/arena.h"
#include "core/array.h"
#include "core/memory_stats.h"
#include "test_support.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>

using de::core::Arena;
using de::core::Array;
using de::core::heap_allocator;
using de::core::NoHeapAllocationScope;
using de::test::CountingAllocator;

namespace {

bool is_aligned(const void* ptr, size_t alignment) {
    return reinterpret_cast<uintptr_t>(ptr) % alignment == 0;
}

} // namespace

TEST_CASE("allocations are aligned and bump forward") {
    Arena arena;
    for (const size_t alignment : {size_t{1}, size_t{2}, size_t{8}, size_t{16}, size_t{64}, size_t{256}}) {
        CHECK(is_aligned(arena.allocate(3, alignment), alignment));
    }
    auto* first = static_cast<std::byte*>(arena.allocate(16, 16));
    void* second = arena.allocate(16, 16);
    CHECK(second == first + 16);
}

TEST_CASE("blocks come from the backing allocator, lazily") {
    CountingAllocator backing;
    {
        Arena arena(backing, 1024);
        CHECK(backing.allocations == 0);
        arena.allocate(100, 8);
        CHECK(backing.allocations == 1);
        for (int i = 0; i < 100; ++i) {
            arena.allocate(100, 8);
        }
        CHECK(backing.allocations > 1);
        CHECK(arena.reserved_bytes() >= arena.used_bytes());
        CHECK(arena.used_bytes() >= 101 * 100);
    }
    CHECK(backing.live_bytes == 0);
}

TEST_CASE("a request larger than the block size gets its own block") {
    Arena arena(heap_allocator(), 1024);
    void* big = arena.allocate(10000, 64);
    CHECK(is_aligned(big, 64));
    std::memset(big, 0xab, 10000); // the whole range is usable
    CHECK(arena.reserved_bytes() >= 10000);
}

TEST_CASE("reset reuses the blocks without touching the backing allocator") {
    CountingAllocator backing;
    Arena arena(backing, 4096);
    for (int i = 0; i < 50; ++i) {
        arena.allocate(256, 16);
    }
    const int allocations = backing.allocations;
    const size_t reserved = arena.reserved_bytes();

    arena.reset();
    CHECK(arena.used_bytes() == 0);
    for (int i = 0; i < 50; ++i) {
        arena.allocate(256, 16);
    }
    CHECK(backing.allocations == allocations);
    CHECK(arena.reserved_bytes() == reserved);
}

TEST_CASE("rewind frees everything allocated after the marker") {
    Arena arena(heap_allocator(), 1024);
    arena.allocate(100, 8);
    const Arena::Marker marker = arena.mark();
    const size_t used = arena.used_bytes();

    void* first = arena.allocate(64, 8);
    for (int i = 0; i < 40; ++i) {
        arena.allocate(100, 8); // crosses into later blocks
    }
    arena.rewind(marker);
    CHECK(arena.used_bytes() == used);
    CHECK(arena.allocate(64, 8) == first);
}

TEST_CASE("rewind to a marker taken before the first block") {
    Arena arena(heap_allocator(), 1024);
    const Arena::Marker empty = arena.mark();
    void* first = arena.allocate(32, 8);
    arena.allocate(2000, 8);
    arena.rewind(empty);
    CHECK(arena.used_bytes() == 0);
    CHECK(arena.allocate(32, 8) == first);
}

TEST_CASE("release returns every block") {
    CountingAllocator backing;
    Arena arena(backing, 1024);
    for (int i = 0; i < 20; ++i) {
        arena.allocate(200, 8);
    }
    arena.release();
    CHECK(backing.live_bytes == 0);
    CHECK(arena.reserved_bytes() == 0);
    CHECK(arena.allocate(8, 8) != nullptr); // still usable
}

TEST_CASE("containers allocate from an arena") {
    Arena arena;
    {
        Array<int> numbers(arena);
        for (int i = 0; i < 1000; ++i) {
            numbers.push(i);
        }
        CHECK(numbers[999] == 999);
    }
    CHECK(arena.used_bytes() >= 1000 * sizeof(int)); // nothing comes back until reset
}

TEST_CASE("a warmed-up arena stays off the heap") {
    Arena arena(heap_allocator(), 4096);
    arena.allocate(3000, 8);
    arena.reset();
    const NoHeapAllocationScope no_alloc;
    for (int i = 0; i < 3; ++i) {
        arena.allocate(1000, 8);
    }
    CHECK(no_alloc.allocations() == 0);
}
