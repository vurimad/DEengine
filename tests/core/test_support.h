#pragma once

#include "core/allocator.h"

#include <cstddef>

namespace de::test {

// Forwards to the heap and counts calls, to check that containers allocate through the
// allocator they were given and give every byte back.
class CountingAllocator final : public core::Allocator {
public:
    void* allocate(size_t size, size_t alignment) override {
        ++allocations;
        live_bytes += size;
        return core::heap_allocator().allocate(size, alignment);
    }

    void deallocate(void* ptr, size_t size, size_t alignment) override {
        ++deallocations;
        live_bytes -= size;
        core::heap_allocator().deallocate(ptr, size, alignment);
    }

    int allocations = 0;
    int deallocations = 0;
    size_t live_bytes = 0;
};

// Counts live instances, to check that containers construct and destroy every element exactly
// once. Moved-from instances hold -1.
struct Tracked {
    static inline int live = 0;

    int value = 0;

    Tracked() { ++live; }
    explicit Tracked(int v) : value(v) { ++live; }
    Tracked(const Tracked& other) : value(other.value) { ++live; }
    Tracked(Tracked&& other) noexcept : value(other.value) {
        other.value = -1;
        ++live;
    }
    Tracked& operator=(const Tracked& other) = default;
    Tracked& operator=(Tracked&& other) noexcept {
        value = other.value;
        other.value = -1;
        return *this;
    }
    ~Tracked() { --live; }
};

// Move-only payload, for containers and Result with non-copyable types.
struct MoveOnly {
    int value = 0;

    explicit MoveOnly(int v) : value(v) {}
    MoveOnly(const MoveOnly&) = delete;
    MoveOnly& operator=(const MoveOnly&) = delete;
    MoveOnly(MoveOnly&& other) noexcept : value(other.value) { other.value = -1; }
    MoveOnly& operator=(MoveOnly&& other) noexcept {
        value = other.value;
        other.value = -1;
        return *this;
    }
    ~MoveOnly() = default;
};

} // namespace de::test
