#include "core/range_allocator.h"

#include "core/assert.h"

#include <algorithm>

namespace de::core {

RangeAllocator::RangeAllocator(Allocator& allocator, uint64_t capacity)
    : free_(allocator), size_(capacity), free_total_(capacity) {
    if (capacity > 0) {
        free_.push({.offset = 0, .size = capacity});
    }
}

uint64_t RangeAllocator::allocate(uint64_t count) {
    DE_ASSERT(count > 0);
    for (size_t i = 0; i < free_.size(); ++i) {
        Range& range = free_[i];
        if (range.size < count) {
            continue;
        }
        const uint64_t offset = range.offset;
        free_total_ -= count;
        if (range.size > count) {
            range.offset += count;
            range.size -= count;
            return offset;
        }
        // Used up: the rest stay in order.
        for (size_t k = i + 1; k < free_.size(); ++k) {
            free_[k - 1] = free_[k];
        }
        free_.resize(free_.size() - 1);
        return offset;
    }
    return kNone;
}

void RangeAllocator::free(uint64_t offset, uint64_t count) {
    DE_ASSERT(count > 0 && offset <= size_ && count <= size_ - offset);
    free_total_ += count;
    size_t at = 0;
    while (at < free_.size() && free_[at].offset < offset) {
        ++at;
    }
    DE_ASSERT_MSG(at == free_.size() || offset + count <= free_[at].offset, "this range overlaps a free one");
    DE_ASSERT_MSG(at == 0 || free_[at - 1].offset + free_[at - 1].size <= offset, "this range overlaps a free one");
    const bool joins_before = at > 0 && free_[at - 1].offset + free_[at - 1].size == offset;
    const bool joins_after = at < free_.size() && offset + count == free_[at].offset;
    if (joins_before && joins_after) {
        free_[at - 1].size += count + free_[at].size;
        for (size_t k = at + 1; k < free_.size(); ++k) {
            free_[k - 1] = free_[k];
        }
        free_.resize(free_.size() - 1);
    } else if (joins_before) {
        free_[at - 1].size += count;
    } else if (joins_after) {
        free_[at].offset = offset;
        free_[at].size += count;
    } else {
        free_.push({});
        for (size_t k = free_.size() - 1; k > at; --k) {
            free_[k] = free_[k - 1];
        }
        free_[at] = {.offset = offset, .size = count};
    }
}

uint64_t RangeAllocator::largest_free() const {
    uint64_t largest = 0;
    for (const Range& range : free_) {
        largest = std::max(largest, range.size);
    }
    return largest;
}

} // namespace de::core
