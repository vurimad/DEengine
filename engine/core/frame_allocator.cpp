#include "core/frame_allocator.h"

#include "core/log.h"

namespace de::core {

namespace {

// Each frame's buffer starts on its own cache line.
constexpr size_t kBufferAlignment = 64;

} // namespace

FrameAllocator::FrameAllocator(size_t bytes_per_frame, Allocator& backing)
    : backing_(&backing), bytes_per_frame_(align_up(bytes_per_frame, kBufferAlignment)),
      buffers_(static_cast<std::byte*>(backing.allocate(bytes_per_frame_ * kFramesInFlight, kBufferAlignment))),
      current_(buffers_) {
    DE_ASSERT_MSG(bytes_per_frame > 0, "frame allocator budget must be positive");
}

FrameAllocator::~FrameAllocator() {
    backing_->deallocate(buffers_, bytes_per_frame_ * kFramesInFlight, kBufferAlignment);
}

void FrameAllocator::begin_frame() {
    const size_t used = offset_.load(std::memory_order_relaxed);
    peak_ = used > peak_ ? used : peak_;
    ++frame_index_;
    current_ = buffers_ + (frame_index_ % kFramesInFlight) * bytes_per_frame_;
    offset_.store(0, std::memory_order_relaxed);
}

void* FrameAllocator::allocate(size_t size, size_t alignment) {
    DE_ASSERT_MSG(is_power_of_two(alignment), "alignment must be a power of two");
    size_t offset = offset_.load(std::memory_order_relaxed);
    for (;;) {
        const size_t start = offset + alignment_padding(current_ + offset, alignment);
        if (start > bytes_per_frame_ || size > bytes_per_frame_ - start) {
            DE_FATAL("memory",
                     "frame allocator out of memory: %zu of %zu bytes used, %zu more requested; raise its budget",
                     offset, bytes_per_frame_, size);
        }
        if (offset_.compare_exchange_weak(offset, start + size, std::memory_order_relaxed)) {
            return current_ + start;
        }
    }
}

size_t FrameAllocator::peak_bytes() const {
    const size_t used = used_bytes();
    return used > peak_ ? used : peak_;
}

} // namespace de::core
