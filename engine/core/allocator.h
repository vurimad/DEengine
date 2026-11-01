#pragma once

#include "core/assert.h"

#include <cstddef>
#include <cstdint>

namespace de::core {

// The interface every container allocates through (docs/design.md, Core library).
// Containers take an Allocator& from day one. Phase 1 only has the heap allocator; arenas,
// pools, the frame allocator and memory tags arrive with the allocators task.
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

// General-purpose process heap. Phase 1: global operator new/delete.
Allocator& heap_allocator();

template <typename T>
T* allocate_array(Allocator& allocator, size_t count) {
    // Checked in every build: a wrapped size would return a block too small for count elements.
    if (count > SIZE_MAX / sizeof(T)) [[unlikely]] {
        assert_failed("count <= SIZE_MAX / sizeof(T)", "allocation size overflows size_t", __FILE__, __LINE__);
    }
    return static_cast<T*>(allocator.allocate(sizeof(T) * count, alignof(T)));
}

template <typename T>
void deallocate_array(Allocator& allocator, T* ptr, size_t count) {
    allocator.deallocate(static_cast<void*>(ptr), sizeof(T) * count, alignof(T));
}

} // namespace de::core
