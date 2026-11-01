#include "core/allocator.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>

using de::core::heap_allocator;

TEST_CASE("heap allocator honors small and large alignments") {
    for (const size_t alignment : {size_t{1}, size_t{8}, size_t{16}, size_t{64}, size_t{256}, size_t{4096}}) {
        void* ptr = heap_allocator().allocate(100, alignment);
        REQUIRE(ptr != nullptr);
        CHECK(reinterpret_cast<uintptr_t>(ptr) % alignment == 0);
        heap_allocator().deallocate(ptr, 100, alignment);
    }
}

TEST_CASE("allocate_array returns storage for count elements of T") {
    struct alignas(32) Wide {
        float lanes[8];
    };
    Wide* items = de::core::allocate_array<Wide>(heap_allocator(), 4);
    CHECK(reinterpret_cast<uintptr_t>(items) % alignof(Wide) == 0);
    de::core::deallocate_array(heap_allocator(), items, 4);
}
