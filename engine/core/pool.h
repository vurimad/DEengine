#pragma once

#include "core/allocator.h"

#include <cstddef>
#include <new>
#include <utility>

namespace de::core {

// Fixed-size block allocator for hot small objects of one type (docs/design.md, Memory).
// O(1) allocate and free through an intrusive free list; freed blocks are reused first.
// Grows a chunk at a time from the backing allocator and keeps its chunks until destroyed.
// Every block must be freed before the pool is destroyed. Not thread-safe. Not movable.
//     Pool enemies(sizeof(Enemy), alignof(Enemy));
//     Enemy* enemy = enemies.create<Enemy>(spawn);  ...  enemies.destroy(enemy);
class Pool final : public Allocator {
public:
    static constexpr size_t kDefaultBlocksPerChunk = 256;

    Pool(size_t block_size, size_t block_alignment, size_t blocks_per_chunk = kDefaultBlocksPerChunk,
         Allocator& backing = heap_allocator());
    ~Pool();

    Pool(Pool&&) = delete;
    Pool& operator=(Pool&&) = delete;

    // size and alignment must fit the pool's blocks.
    void* allocate(size_t size, size_t alignment) override;
    void deallocate(void* ptr, size_t size, size_t alignment) override;

    template <typename T, typename... Args>
    T* create(Args&&... args) {
        return ::new (allocate(sizeof(T), alignof(T))) T(std::forward<Args>(args)...);
    }

    template <typename T>
    void destroy(T* object) {
        if (object != nullptr) {
            object->~T();
            deallocate(static_cast<void*>(object), sizeof(T), alignof(T));
        }
    }

    // The block stride: the requested size rounded up to the alignment and to fit a free-list link.
    size_t block_size() const { return block_stride_; }
    size_t live_blocks() const { return live_blocks_; }
    size_t capacity_blocks() const { return capacity_blocks_; }

private:
    struct FreeBlock {
        FreeBlock* next;
    };
    struct Chunk {
        Chunk* next;
    };

    size_t chunk_bytes() const { return chunk_header_ + block_stride_ * blocks_per_chunk_; }
    void add_chunk();

    Allocator* backing_;
    size_t block_alignment_;
    size_t block_stride_;
    size_t blocks_per_chunk_;
    size_t chunk_header_;
    FreeBlock* free_list_ = nullptr;
    Chunk* chunks_ = nullptr;
    size_t live_blocks_ = 0;
    size_t capacity_blocks_ = 0;
};

} // namespace de::core
