#include "core/arena.h"

#include <cstdint>
#include <new>

namespace de::core {

namespace {

constexpr size_t kBlockAlignment = alignof(std::max_align_t);

} // namespace

Arena::Arena(Allocator& backing, size_t block_size) : backing_(&backing), block_size_(block_size) {
    DE_ASSERT_MSG(block_size > 0, "arena block size must be positive");
}

Arena::~Arena() {
    release();
}

std::byte* Arena::block_data(Block* block) {
    return reinterpret_cast<std::byte*>(block) + align_up(sizeof(Block), kBlockAlignment);
}

void* Arena::bump(size_t size, size_t alignment) {
    std::byte* data = block_data(current_);
    const size_t remaining = current_->capacity - offset_;
    const size_t padding = alignment_padding(data + offset_, alignment);
    if (padding > remaining || size > remaining - padding) {
        return nullptr;
    }
    std::byte* result = data + offset_ + padding;
    offset_ += padding + size;
    used_ += padding + size;
    return result;
}

void* Arena::allocate(size_t size, size_t alignment) {
    DE_ASSERT_MSG(is_power_of_two(alignment), "alignment must be a power of two");
    DE_VERIFY_MSG(size <= SIZE_MAX / 2, "arena allocation size is not plausible");

    if (current_ != nullptr) {
        if (void* result = bump(size, alignment)) {
            return result;
        }
        // Move on through blocks kept from before a reset or rewind. Blocks too small for this
        // request are skipped until the next reset.
        for (Block* block = current_->next; block != nullptr; block = block->next) {
            current_ = block;
            offset_ = 0;
            if (void* result = bump(size, alignment)) {
                return result;
            }
        }
    }

    current_ = add_block(size + alignment);
    offset_ = 0;
    void* result = bump(size, alignment);
    DE_ASSERT(result != nullptr);
    return result;
}

Arena::Block* Arena::add_block(size_t minimum_capacity) {
    const size_t capacity = minimum_capacity > block_size_ ? minimum_capacity : block_size_;
    const size_t header = align_up(sizeof(Block), kBlockAlignment);
    void* memory = backing_->allocate(header + capacity, kBlockAlignment);
    auto* block = ::new (memory) Block{.next = nullptr, .capacity = capacity};
    if (last_ != nullptr) {
        last_->next = block;
    } else {
        first_ = block;
    }
    last_ = block;
    reserved_ += header + capacity;
    return block;
}

void Arena::rewind(const Marker& marker) {
    DE_ASSERT_MSG(marker.used <= used_, "rewinding forward, or to another arena's marker");
    if (marker.block != nullptr) {
        current_ = marker.block;
        offset_ = marker.offset;
    } else {
        current_ = first_;
        offset_ = 0;
    }
    used_ = marker.used;
}

void Arena::reset() {
    current_ = first_;
    offset_ = 0;
    used_ = 0;
}

void Arena::release() {
    const size_t header = align_up(sizeof(Block), kBlockAlignment);
    Block* block = first_;
    while (block != nullptr) {
        Block* next = block->next;
        backing_->deallocate(block, header + block->capacity, kBlockAlignment);
        block = next;
    }
    first_ = nullptr;
    last_ = nullptr;
    current_ = nullptr;
    offset_ = 0;
    used_ = 0;
    reserved_ = 0;
}

} // namespace de::core
