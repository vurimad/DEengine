#include "core/allocator.h"

#include "core/memory_stats.h"

#include <atomic>
#include <new>
#include <utility>

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
constinit thread_local uint64_t thread_heap_allocations = 0;

// Checked in every build: a bad tag (cast from data) would index past the heaps and counters.
size_t tag_index(MemoryTag tag) {
    const auto index = static_cast<size_t>(tag);
    DE_VERIFY_MSG(index < kMemoryTagCount, "invalid memory tag");
    return index;
}

void record_allocation(size_t tag, size_t size) {
    TagCounters& counters = tag_counters[tag];
    const uint64_t live = counters.live_bytes.fetch_add(size, std::memory_order_relaxed) + size;
    counters.live_allocations.fetch_add(1, std::memory_order_relaxed);
    counters.total_allocations.fetch_add(1, std::memory_order_relaxed);
    uint64_t peak = counters.peak_bytes.load(std::memory_order_relaxed);
    while (live > peak && !counters.peak_bytes.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {
    }
    ++thread_heap_allocations;
}

void record_deallocation(size_t tag, size_t size) {
    TagCounters& counters = tag_counters[tag];
    counters.live_bytes.fetch_sub(size, std::memory_order_relaxed);
    counters.live_allocations.fetch_sub(1, std::memory_order_relaxed);
}

class HeapAllocator final : public Allocator {
public:
    explicit constexpr HeapAllocator(MemoryTag tag) : tag_(static_cast<size_t>(tag)) {}

    void* allocate(size_t size, size_t alignment) override {
        DE_ASSERT_MSG(is_power_of_two(alignment), "alignment must be a power of two");
        void* ptr = alignment <= __STDCPP_DEFAULT_NEW_ALIGNMENT__ ? ::operator new(size)
                                                                  : ::operator new(size, std::align_val_t{alignment});
        record_allocation(tag_, size);
        return ptr;
    }

    void deallocate(void* ptr, size_t size, size_t alignment) override {
        record_deallocation(tag_, size);
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

private:
    size_t tag_;
};

template <typename Indices>
struct TaggedHeaps;

template <size_t... Index>
struct TaggedHeaps<std::index_sequence<Index...>> {
    HeapAllocator heaps[sizeof...(Index)] = {HeapAllocator(static_cast<MemoryTag>(Index))...};
};

// Constant-initialized and trivially destructible: no initialization guard on each call, and
// usable during static initialization and destruction.
constinit TaggedHeaps<std::make_index_sequence<kMemoryTagCount>> tagged_heaps;

} // namespace

Allocator& heap_allocator(MemoryTag tag) {
    return tagged_heaps.heaps[tag_index(tag)];
}

MemoryStats memory_stats(MemoryTag tag) {
    const TagCounters& counters = tag_counters[tag_index(tag)];
    return MemoryStats{
        .live_bytes = counters.live_bytes.load(std::memory_order_relaxed),
        .peak_bytes = counters.peak_bytes.load(std::memory_order_relaxed),
        .live_allocations = counters.live_allocations.load(std::memory_order_relaxed),
        .total_allocations = counters.total_allocations.load(std::memory_order_relaxed),
    };
}

uint64_t heap_allocation_count() {
    // Summed on read (rare) rather than kept in one more atomic that every allocation would hit.
    uint64_t total = 0;
    for (const TagCounters& counters : tag_counters) {
        total += counters.total_allocations.load(std::memory_order_relaxed);
    }
    return total;
}

uint64_t thread_heap_allocation_count() {
    return thread_heap_allocations;
}

} // namespace de::core
