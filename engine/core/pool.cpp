#include "core/pool.h"

namespace de::core {

namespace {

constexpr size_t max_of(size_t a, size_t b) {
    return a > b ? a : b;
}

} // namespace

Pool::Pool(size_t block_size, size_t block_alignment, size_t blocks_per_chunk, Allocator& backing)
    : backing_(&backing), block_alignment_(max_of(block_alignment, alignof(FreeBlock))),
      block_stride_(align_up(max_of(block_size, sizeof(FreeBlock)), block_alignment_)),
      blocks_per_chunk_(blocks_per_chunk), chunk_header_(align_up(sizeof(Chunk), block_alignment_)) {
    DE_ASSERT_MSG(is_power_of_two(block_alignment), "alignment must be a power of two");
    DE_ASSERT_MSG(blocks_per_chunk > 0, "a chunk needs at least one block");
}

Pool::~Pool() {
    DE_ASSERT_MSG(live_blocks_ == 0, "Pool destroyed while blocks are still allocated");
    Chunk* chunk = chunks_;
    while (chunk != nullptr) {
        Chunk* next = chunk->next;
        backing_->deallocate(chunk, chunk_bytes(), block_alignment_);
        chunk = next;
    }
}

void* Pool::allocate(size_t size, size_t alignment) {
    DE_ASSERT_MSG(size <= block_stride_ && alignment <= block_alignment_, "request does not fit the pool's blocks");
    if (free_list_ == nullptr) {
        add_chunk();
    }
    FreeBlock* block = free_list_;
    free_list_ = block->next;
    ++live_blocks_;
    return block;
}

void Pool::deallocate(void* ptr, size_t /*size*/, size_t /*alignment*/) {
    DE_ASSERT(ptr != nullptr);
    DE_ASSERT(live_blocks_ > 0);
    free_list_ = ::new (ptr) FreeBlock{free_list_};
    --live_blocks_;
}

void Pool::add_chunk() {
    void* memory = backing_->allocate(chunk_bytes(), block_alignment_);
    chunks_ = ::new (memory) Chunk{chunks_};
    std::byte* blocks = static_cast<std::byte*>(memory) + chunk_header_;
    // Pushed in reverse, so blocks are handed out in address order.
    for (size_t i = blocks_per_chunk_; i-- > 0;) {
        free_list_ = ::new (static_cast<void*>(blocks + i * block_stride_)) FreeBlock{free_list_};
    }
    capacity_blocks_ += blocks_per_chunk_;
}

} // namespace de::core
