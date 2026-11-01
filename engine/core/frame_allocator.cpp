#include "core/frame_allocator.h"

#include "core/log.h"

#include <algorithm>
#include <cstdint>

namespace de::core {

namespace {

// Each frame's buffer starts on its own cache line.
constexpr size_t kBufferAlignment = 64;

// Checked in every build: an overflowing budget would size the buffer smaller than the
// allocator believes, and it would hand out memory past the end.
size_t checked_budget(size_t bytes_per_frame) {
    DE_VERIFY_MSG(bytes_per_frame > 0 && bytes_per_frame <= SIZE_MAX / (2 * FrameAllocator::kMaxFramesInFlight),
                  "frame allocator budget is zero or not plausible");
    return align_up(bytes_per_frame, kBufferAlignment);
}

size_t checked_frames_in_flight(size_t frames_in_flight) {
    DE_VERIFY_MSG(frames_in_flight >= 1 && frames_in_flight <= FrameAllocator::kMaxFramesInFlight,
                  "frames in flight must be between 1 and kMaxFramesInFlight");
    return frames_in_flight;
}

} // namespace

FrameAllocator::FrameAllocator(size_t bytes_per_frame, size_t frames_in_flight, Allocator& backing)
    : backing_(&backing), bytes_per_frame_(checked_budget(bytes_per_frame)),
      frames_in_flight_(checked_frames_in_flight(frames_in_flight)),
      buffers_(static_cast<std::byte*>(backing.allocate(bytes_per_frame_ * frames_in_flight_, kBufferAlignment))),
      current_(buffers_) {}

FrameAllocator::~FrameAllocator() {
    backing_->deallocate(buffers_, bytes_per_frame_ * frames_in_flight_, kBufferAlignment);
}

void FrameAllocator::begin_frame() {
    peak_ = std::max(peak_, offset_.load(std::memory_order_relaxed));
    ++frame_index_;
    current_ = buffers_ + (frame_index_ % frames_in_flight_) * bytes_per_frame_;
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
    return std::max(peak_, used_bytes());
}

} // namespace de::core
