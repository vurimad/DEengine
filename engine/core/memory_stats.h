#pragma once

#include "core/assert.h"
#include "core/memory_tag.h"

#include <cstdint>

namespace de::core {

// Heap usage of one memory tag. Arenas, pools and the frame allocator count the blocks they
// hold from the heap, not what they hand out. Thread-safe snapshot: the fields are read one
// by one, not as a single transaction.
struct MemoryStats {
    uint64_t live_bytes = 0;
    uint64_t peak_bytes = 0;
    uint64_t live_allocations = 0;
    uint64_t total_allocations = 0;
};

MemoryStats memory_stats(MemoryTag tag);

// Heap allocations made so far, through any tagged heap (monotonic). Diff them around a frame
// to check "zero heap allocations per frame in steady state" (docs/design.md, Performance rules).
uint64_t heap_allocation_count();        // every thread
uint64_t thread_heap_allocation_count(); // the calling thread only

// Dev builds: asserts that the calling thread made no heap allocation while the scope was alive.
//     { const NoHeapAllocationScope no_alloc; simulate(tick); }
class NoHeapAllocationScope {
public:
    NoHeapAllocationScope() : start_(thread_heap_allocation_count()) {}
    ~NoHeapAllocationScope() {
        DE_ASSERT_MSG(thread_heap_allocation_count() == start_, "heap allocation inside a NoHeapAllocationScope");
    }

    NoHeapAllocationScope(const NoHeapAllocationScope&) = delete;
    NoHeapAllocationScope& operator=(const NoHeapAllocationScope&) = delete;
    NoHeapAllocationScope(NoHeapAllocationScope&&) = delete;
    NoHeapAllocationScope& operator=(NoHeapAllocationScope&&) = delete;

    uint64_t allocations() const { return thread_heap_allocation_count() - start_; }

private:
    uint64_t start_;
};

} // namespace de::core
