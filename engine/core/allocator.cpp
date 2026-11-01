#include "core/allocator.h"

#include "core/memory_stats.h"

#include <atomic>
#include <new>

namespace de::core {

namespace {

// One cache line per tag: different threads update different tags.
struct alignas(64) TagCounters {
    std::atomic<uint64_t> live_bytes{0};
    std::atomic<uint64_t> peak_bytes{0};
    std::atomic<uint64_t> live_allocations{0};
    std::atomic<uint64_t> total_allocations{0};
};

// Constant-initialized, so allocations made during static initialization are counted correctly.
constinit TagCounters tag_counters[kMemoryTagCount] = {};
alignas(64) constinit std::atomic<uint64_t> total_heap_allocations{0};
constinit thread_local uint64_t thread_heap_allocations = 0;

void record_allocation(MemoryTag tag, size_t size) {
    TagCounters& counters = tag_counters[static_cast<size_t>(tag)];
    const uint64_t live = counters.live_bytes.fetch_add(size, std::memory_order_relaxed) + size;
    counters.live_allocations.fetch_add(1, std::memory_order_relaxed);
    counters.total_allocations.fetch_add(1, std::memory_order_relaxed);
    uint64_t peak = counters.peak_bytes.load(std::memory_order_relaxed);
    while (live > peak && !counters.peak_bytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
    total_heap_allocations.fetch_add(1, std::memory_order_relaxed);
    ++thread_heap_allocations;
}

void record_deallocation(MemoryTag tag, size_t size) {
    TagCounters& counters = tag_counters[static_cast<size_t>(tag)];
    counters.live_bytes.fetch_sub(size, std::memory_order_relaxed);
    counters.live_allocations.fetch_sub(1, std::memory_order_relaxed);
}

class HeapAllocator final : public Allocator {
public:
    void* allocate(size_t size, size_t alignment) override {
        DE_ASSERT_MSG(is_power_of_two(alignment), "alignment must be a power of two");
        void* ptr = alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__ ? ::operator new(size)
                                                                  : ::operator new(size, std::align_val_t{alignment});
        record_allocation(tag, size);
        return ptr;
    }

    void deallocate(void* ptr, size_t size, size_t alignment) override {
        record_deallocation(tag, size);
#if defined(__cpp_sized_deallocation)
        if (alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
            ::operator delete(ptr, size);
            return;
        }
        ::operator delete(ptr, size, std::align_val_t{alignment});
#else
        // Sized deallocation is off by default before clang 19.
        if (alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__) {
            ::operator delete(ptr);
            return;
        }
        ::operator delete(ptr, std::align_val_t{alignment});
#endif
    }

    MemoryTag tag = MemoryTag::General;
};

struct TaggedHeaps {
    HeapAllocator heaps[kMemoryTagCount];

    TaggedHeaps() {
        for (size_t i = 0; i < kMemoryTagCount; ++i) {
            heaps[i].tag = static_cast<MemoryTag>(i);
        }
    }
};

} // namespace

Allocator& heap_allocator(MemoryTag tag) {
    DE_ASSERT(static_cast<size_t>(tag) < kMemoryTagCount);
    // Stateless and trivially destructible, so the heaps stay usable during static destruction.
    static TaggedHeaps heaps;
    return heaps.heaps[static_cast<size_t>(tag)];
}

MemoryStats memory_stats(MemoryTag tag) {
    DE_ASSERT(static_cast<size_t>(tag) < kMemoryTagCount);
    const TagCounters& counters = tag_counters[static_cast<size_t>(tag)];
    return MemoryStats{
        .live_bytes = counters.live_bytes.load(std::memory_order_relaxed),
        .peak_bytes = counters.peak_bytes.load(std::memory_order_relaxed),
        .live_allocations = counters.live_allocations.load(std::memory_order_relaxed),
        .total_allocations = counters.total_allocations.load(std::memory_order_relaxed),
    };
}

uint64_t heap_allocation_count() {
    return total_heap_allocations.load(std::memory_order_relaxed);
}

uint64_t thread_heap_allocation_count() {
    return thread_heap_allocations;
}

} // namespace de::core
