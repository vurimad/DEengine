#pragma once

#include "core/assert.h"
#include "core/memory_tag.h"

#include <cstddef>
#include <cstdint>

namespace de::core {

constexpr bool is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1)) == 0;
}

// Rounds value up to a multiple of alignment (a power of two).
constexpr size_t align_up(size_t value, size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

// Bytes to add to ptr to reach the next multiple of alignment.
inline size_t alignment_padding(const void* ptr, size_t alignment) {
    const auto address = reinterpret_cast<uintptr_t>(ptr);
    return align_up(address, alignment) - address;
}

// The interface every container allocates through (docs/design.md, Core library).
// Implementations: the tagged heaps (heap_allocator), Arena, Pool and FrameAllocator.
class Allocator {
public:
    Allocator(const Allocator&) = delete;
    Allocator& operator=(const Allocator&) = delete;

    // alignment is a power of two. Never returns null (out of memory aborts).
    virtual void* allocate(size_t size, size_t alignment) = 0;

    // size and alignment must match the allocate call, so arenas and pools need no headers.
    virtual void deallocate(void* ptr, size_t size, size_t alignment) = 0;

protected:
    Allocator() = default;
    ~Allocator() = default;
    Allocator(Allocator&&) = default;
    Allocator& operator=(Allocator&&) = default;
};

// The process heap: one allocator per memory tag, so every heap byte is attributed to a
// system (memory_stats.h). Thread-safe. Phase 1: global operator new/delete.
Allocator& heap_allocator(MemoryTag tag = MemoryTag::General);

template <typename T>
T* allocate_array(Allocator& allocator, size_t count) {
    // Checked in every build: a wrapped size would return a block too small for count elements.
    DE_VERIFY_MSG(count <= SIZE_MAX / sizeof(T), "allocation size overflows size_t");
    return static_cast<T*>(allocator.allocate(sizeof(T) * count, alignof(T)));
}

template <typename T>
void deallocate_array(Allocator& allocator, T* ptr, size_t count) {
    allocator.deallocate(static_cast<void*>(ptr), sizeof(T) * count, alignof(T));
}

} // namespace de::core
