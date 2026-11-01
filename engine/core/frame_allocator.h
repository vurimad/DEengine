#pragma once

#include "core/allocator.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace de::core {

// Per-frame scratch memory, freed wholesale at the start of each frame (docs/design.md, Memory).
//
// One buffer per frame in flight rotates, so data allocated during frame N stays valid until
// begin_frame() has been called frames_in_flight times. Choose the count from who reads the data:
//   2: the render thread consumes frame N's data during frame N+1 (game -> render handoff)
//   3: a three-stage pipeline (game N+1, render N, GPU N-1) where the data lives until the GPU
//      has finished with its frame
// This is the renderer's frames-in-flight count, not the swapchain image count: those often
// differ (e.g. 3 swapchain images, 2 frames in flight).
//
// Any thread may allocate (lock-free bump); begin_frame() runs on one thread while no
// allocation is in flight. Fixed budget, reserved up front: running out is fatal with a message
// naming the budget, so per-frame memory stays predictable. deallocate is a no-op. Not movable.
class FrameAllocator final : public Allocator {
public:
    static constexpr size_t kDefaultFramesInFlight = 2;
    static constexpr size_t kMaxFramesInFlight = 3;

    explicit FrameAllocator(size_t bytes_per_frame, size_t frames_in_flight = kDefaultFramesInFlight,
                            Allocator& backing = heap_allocator(MemoryTag::Core));
    ~FrameAllocator();

    FrameAllocator(FrameAllocator&&) = delete;
    FrameAllocator& operator=(FrameAllocator&&) = delete;

    // Starts the next frame: switches to the next buffer and frees everything in it.
    void begin_frame();

    void* allocate(size_t size, size_t alignment) override;
    void deallocate(void* /*ptr*/, size_t /*size*/, size_t /*alignment*/) override {}

    // Bytes used in the current frame, including alignment padding.
    size_t used_bytes() const { return offset_.load(std::memory_order_relaxed); }

    // The most any frame has used so far: compare it with the budget when tuning.
    size_t peak_bytes() const;

    size_t bytes_per_frame() const { return bytes_per_frame_; }
    size_t frames_in_flight() const { return frames_in_flight_; }

    // Number of begin_frame() calls so far.
    uint64_t frame_index() const { return frame_index_; }

private:
    Allocator* backing_;
    size_t bytes_per_frame_;
    size_t frames_in_flight_;
    std::byte* buffers_;
    std::byte* current_;
    std::atomic<size_t> offset_{0};
    size_t peak_ = 0;
    uint64_t frame_index_ = 0;
};

} // namespace de::core
