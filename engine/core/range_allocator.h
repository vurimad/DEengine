#pragma once

#include "core/allocator.h"
#include "core/array.h"

#include <cstdint>

namespace de::core {

// Hands out ranges of something that isn't memory it owns, like the vertices of a big GPU buffer: first
// fit, freed ranges merging with their neighbours. Linear in the free ranges, so for what happens when
// assets load and unload, not per frame or per instance. The caller remembers the sizes, and keeps a
// range from being reused while something may still read it.
class RangeAllocator {
public:
    static constexpr uint64_t kNone = UINT64_MAX;

    RangeAllocator(Allocator& allocator, uint64_t capacity);

    // The start of a free range this long (above 0), or kNone if no free range holds it.
    uint64_t allocate(uint64_t count);
    // A range allocate() returned, whole.
    void free(uint64_t offset, uint64_t count);

    uint64_t size() const { return size_; }
    uint64_t free_total() const { return free_total_; }
    // What the next allocate() can get: less than free_total() when free space is in pieces.
    uint64_t largest_free() const;

private:
    struct Range {
        uint64_t offset = 0;
        uint64_t size = 0;
    };

    Array<Range> free_; // by offset, none touching
    uint64_t size_ = 0;
    uint64_t free_total_ = 0;
};

} // namespace de::core
