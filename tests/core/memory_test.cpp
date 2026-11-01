#include "core/allocator.h"
#include "core/inline_array.h"
#include "core/memory_stats.h"

#include <doctest/doctest.h>

#include <cstring>
#include <thread>

using de::core::heap_allocator;
using de::core::InlineArray;
using de::core::memory_stats;
using de::core::MemoryStats;
using de::core::MemoryTag;
using de::core::NoHeapAllocationScope;

TEST_CASE("tagged heaps attribute bytes to their own tag only") {
    // Audio and Navigation are unused by the rest of the test suite, so the numbers are exact.
    const MemoryStats before = memory_stats(MemoryTag::Audio);
    const MemoryStats other_before = memory_stats(MemoryTag::Navigation);

    de::core::Allocator& audio = heap_allocator(MemoryTag::Audio);
    void* a = audio.allocate(1000, 16);
    void* b = audio.allocate(24, 8);

    const MemoryStats during = memory_stats(MemoryTag::Audio);
    CHECK(during.live_bytes - before.live_bytes == 1024);
    CHECK(during.live_allocations - before.live_allocations == 2);
    CHECK(during.total_allocations - before.total_allocations == 2);
    CHECK(during.peak_bytes >= during.live_bytes);

    audio.deallocate(b, 24, 8);
    audio.deallocate(a, 1000, 16);

    const MemoryStats after = memory_stats(MemoryTag::Audio);
    CHECK(after.live_bytes == before.live_bytes);
    CHECK(after.live_allocations == before.live_allocations);
    CHECK(after.total_allocations == during.total_allocations);
    CHECK(after.peak_bytes >= before.live_bytes + 1024);

    const MemoryStats other_after = memory_stats(MemoryTag::Navigation);
    CHECK(other_after.total_allocations == other_before.total_allocations);
}

TEST_CASE("each tag has its own heap allocator") {
    CHECK(&heap_allocator(MemoryTag::Render) != &heap_allocator(MemoryTag::Audio));
    CHECK(&heap_allocator() == &heap_allocator(MemoryTag::General));
}

TEST_CASE("every tag has a name") {
    for (size_t i = 0; i < de::core::kMemoryTagCount; ++i) {
        CHECK(std::strlen(de::core::memory_tag_name(static_cast<MemoryTag>(i))) > 0);
    }
    CHECK(std::strcmp(de::core::memory_tag_name(MemoryTag::Render), "render") == 0);
}

TEST_CASE("heap allocation counters count per process and per thread") {
    const uint64_t process_before = de::core::heap_allocation_count();
    const uint64_t thread_before = de::core::thread_heap_allocation_count();

    void* ptr = heap_allocator().allocate(8, 8);
    heap_allocator().deallocate(ptr, 8, 8);
    CHECK(de::core::thread_heap_allocation_count() - thread_before == 1);
    CHECK(de::core::heap_allocation_count() - process_before >= 1);

    const uint64_t this_thread = de::core::thread_heap_allocation_count();
    std::thread worker([] {
        void* other = heap_allocator().allocate(8, 8);
        heap_allocator().deallocate(other, 8, 8);
    });
    worker.join();
    CHECK(de::core::thread_heap_allocation_count() == this_thread); // the worker's allocation isn't ours
    CHECK(de::core::heap_allocation_count() - process_before >= 2);
}

TEST_CASE("NoHeapAllocationScope sees code that stays off the heap") {
    InlineArray<int, 8> numbers;
    const NoHeapAllocationScope no_alloc;
    for (int i = 0; i < 8; ++i) {
        numbers.push(i);
    }
    CHECK(no_alloc.allocations() == 0);
}

TEST_CASE("NoHeapAllocationScope counts allocations made inside it") {
    // Counted with a plain counter diff here: the scope itself would assert at the end.
    const uint64_t before = de::core::thread_heap_allocation_count();
    InlineArray<int, 2> numbers;
    for (int i = 0; i < 3; ++i) {
        numbers.push(i); // the third push spills to the heap
    }
    CHECK(de::core::thread_heap_allocation_count() - before == 1);
}
