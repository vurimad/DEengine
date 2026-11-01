#pragma once

#include "core/allocator.h"

#include <cstddef>

namespace de::core {

// Linear allocator: bumps a pointer through blocks taken from a backing allocator and frees
// everything at once (docs/design.md, Memory). For data that dies together: a streamed cell,
// a level load, a tool pass, or scoped scratch with mark() / rewind().
//
// deallocate is a no-op: memory comes back on reset(), rewind() or destruction. Containers work
// on an arena, but growth leaves the old buffers behind until then, so reserve up front.
// Not thread-safe. Not movable: containers keep a pointer to their allocator.
class Arena final : public Allocator {
    struct Block;

public:
    static constexpr size_t kDefaultBlockSize = size_t{64} * 1024;

    // A position to rewind to. Valid for the arena that made it, until reset() or release().
    struct Marker {
        Block* block = nullptr;
        size_t offset = 0;
        size_t used = 0;
    };

    Arena() : Arena(heap_allocator()) {}
    explicit Arena(Allocator& backing, size_t block_size = kDefaultBlockSize);
    ~Arena();

    Arena(Arena&&) = delete;
    Arena& operator=(Arena&&) = delete;

    void* allocate(size_t size, size_t alignment) override;
    void deallocate(void* /*ptr*/, size_t /*size*/, size_t /*alignment*/) override {}

    Marker mark() const { return Marker{.block = current_, .offset = offset_, .used = used_}; }

    // Frees everything allocated after marker; keeps the blocks.
    void rewind(const Marker& marker);

    // Frees every allocation; keeps the blocks for reuse.
    void reset();

    // Frees every allocation and returns the blocks to the backing allocator.
    void release();

    // Bytes handed out since the last reset, including alignment padding.
    size_t used_bytes() const { return used_; }

    // Bytes held from the backing allocator.
    size_t reserved_bytes() const { return reserved_; }

private:
    struct Block {
        Block* next = nullptr;
        size_t capacity = 0; // usable bytes after the header
    };

    static constexpr size_t kBlockAlignment = alignof(std::max_align_t);
    static constexpr size_t kBlockHeaderSize = align_up(sizeof(Block), kBlockAlignment);

    static std::byte* block_data(Block* block);

    // Allocates from current_, or returns null if the request doesn't fit.
    void* bump(size_t size, size_t alignment);

    // Appends a block with at least minimum_capacity usable bytes.
    Block* add_block(size_t minimum_capacity);

    Allocator* backing_;
    size_t block_size_;
    Block* first_ = nullptr;
    Block* last_ = nullptr;
    Block* current_ = nullptr; // null only while the arena holds no blocks
    size_t offset_ = 0;        // bytes used in current_
    size_t used_ = 0;
    size_t reserved_ = 0;
};

} // namespace de::core
