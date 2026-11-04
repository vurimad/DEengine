#pragma once

#include "core/fixed_string.h"
#include "core/result.h"
#include "render/rhi/command_list.h"
#include "render/rhi/rhi.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::platform {
class Window;
}

namespace de::rhi {

struct DeviceImpl;

// What GPU memory goes to, by how resources are used (Device::memory_stats). Swapchain images
// aren't counted (the presentation engine owns them), nor the device's own small internals.
enum class GpuMemoryCategory : uint8_t {
    RenderTargets,   // textures with RenderTarget or DepthStencil usage
    StorageTextures, // other textures with Storage usage
    Textures,        // other textures: sampled assets
    GpuBuffers,      // buffers in GPU-only memory
    UploadBuffers,   // buffers the CPU writes: staging, per-frame data
    ReadbackBuffers, // buffers the CPU reads back
    Count,
};
const char* gpu_memory_category_name(GpuMemoryCategory category);

struct GpuMemoryStats {
    // Live resources per category: allocation sizes (alignment and padding included) and counts.
    uint64_t bytes[static_cast<size_t>(GpuMemoryCategory::Count)] = {};
    uint32_t counts[static_cast<size_t>(GpuMemoryCategory::Count)] = {};
    uint64_t pending_destroy_bytes = 0; // destroyed, freed once the GPU is done with them
    // From the driver, for this process across GPU-local heaps: what it allocated, and what it may
    // use before paging. With budget_from_os (VK_EXT_memory_budget) the OS's numbers, counting
    // every allocation of the process; without, an estimate: 80% of the heaps, VMA's own
    // allocations only. The same numbers as DeviceStats::gpu_memory_*.
    uint64_t device_local_used_bytes = 0;
    uint64_t device_local_budget_bytes = 0;
    bool budget_from_os = false;

    uint64_t total_bytes() const {
        uint64_t total = 0;
        for (const uint64_t category : bytes) {
            total += category;
        }
        return total;
    }
    uint64_t category_bytes(GpuMemoryCategory category) const { return bytes[static_cast<size_t>(category)]; }
    uint32_t category_count(GpuMemoryCategory category) const { return counts[static_cast<size_t>(category)]; }
};

// One live texture's memory (Device::largest_textures).
struct TextureMemory {
    TextureHandle texture;
    TextureDesc desc;           // debug_name is null: the name is below
    core::FixedString<63> name; // the debug name, cut at 63 characters
    uint64_t bytes = 0;
    GpuMemoryCategory category = GpuMemoryCategory::Textures;
};

struct DeviceDesc {
    const char* app_name = "DEengine";
    // The window to present to; it must outlive the device. Null makes a headless device without
    // a swapchain, for tests, tools and the headless mode.
    platform::Window* window = nullptr;
    // The validation layer, if installed (it comes with the Vulkan SDK). Its messages go to the
    // log under "vulkan" and are counted in DeviceStats.
    bool validation = DE_BUILD_DEV != 0;
    // Adds synchronization validation (missing or wrong barriers). Slow: for tests and hunting hazards.
    bool sync_validation = false;
    // Adds GPU-assisted validation: shaders check their pointer accesses (out of bounds) and more
    // as they run. Very slow. It doesn't check image layouts in the bindless tables: a texture
    // sampled in the wrong state goes unreported (see render/rhi/README.md).
    bool gpu_validation = false;
    bool vsync = true;
};

struct DeviceStats {
    uint64_t frame_index = 0;   // frames begun so far
    uint32_t live_textures = 0; // swapchain textures included
    uint32_t live_buffers = 0;
    uint32_t live_pipelines = 0;
    uint32_t bindless_textures = 0;         // sampled texture entries in use (the fallback included)
    uint32_t bindless_storage_textures = 0; // storage texture entries in use (one per mip, the fallback included)
    uint32_t pending_destroys = 0;          // destroyed resources waiting for the GPU to finish with them
    uint32_t swapchain_recreations = 0;
    uint32_t validation_errors = 0;
    uint32_t validation_warnings = 0;
    uint64_t gpu_memory_used_bytes = 0;   // this process, across GPU-local heaps
    uint64_t gpu_memory_budget_bytes = 0; // what the OS lets this process use before it starts paging
};

// The GPU: resources, frames and submission. The only door to the graphics API.
//
// One Device per process. Not thread-safe: create resources, record and submit from one thread,
// the one that renders (the main thread today). Move-only.
//
// A frame:
//     device.begin_frame();
//     TextureHandle backbuffer = device.acquire_swapchain_texture(); // invalid while minimized
//     CommandList list = device.begin_command_list();
//     ... record ...
//     device.end_frame(); // submits every list begun this frame, then presents
class Device {
public:
    static core::Result<Device> create(const DeviceDesc& desc = {});

    ~Device();
    Device(Device&& other) noexcept;
    Device& operator=(Device&& other) noexcept;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    const Capabilities& capabilities() const;
    DeviceStats stats() const;

    // GPU memory by category, kept as resources come and go, and the driver's budget. Cheap
    // enough to poll every frame. (The over-budget warning is begin_frame's, not this.)
    GpuMemoryStats memory_stats() const;
    // The largest live textures, biggest first: fills out and returns how many it filled. Walks
    // every texture: for reports and debug views, not every frame.
    size_t largest_textures(std::span<TextureMemory> out) const;
    // Logs memory_stats() and the largest textures, as many as asked ("render" channel).
    void log_memory_report(size_t largest = 10) const;
    bool validation_enabled() const;

    // --- Resources ---

    core::Result<TextureHandle> create_texture(const TextureDesc& desc);
    core::Result<BufferHandle> create_buffer(const BufferDesc& desc);
    // Fails if the driver rejects the shader code or the state.
    core::Result<PipelineHandle> create_graphics_pipeline(const GraphicsPipelineDesc& desc);
    core::Result<PipelineHandle> create_compute_pipeline(const ComputePipelineDesc& desc);

    // The handle stops resolving at once; the GPU memory is freed when the frames in flight that
    // may still use it are done. Invalid handles are ignored. Swapchain textures can't be destroyed.
    void destroy(TextureHandle texture);
    void destroy(BufferHandle buffer);
    void destroy(PipelineHandle pipeline);

    // The texture's bindless index, for shaders (TextureUsage::Sampled): its entry in the table of
    // its shape (BindlessTable). kInvalidBindlessIndex without that usage or for a stale handle.
    // Sample it in ShaderRead state (DepthRead | ShaderRead for a depth buffer). The index stays
    // the texture's until it is destroyed, and is reused only once the GPU is done with it.
    uint32_t texture_index(TextureHandle texture) const;
    // The bindless index of one mip as a storage texture (TextureUsage::Storage), every layer.
    // Read and write it in ShaderWrite state. Integer formats use the Storage2DUint table (2D only).
    uint32_t storage_texture_index(TextureHandle texture, uint32_t mip = 0) const;

    // The description the texture was created with (debug_name excluded), or null if the handle is stale.
    const TextureDesc* texture_desc(TextureHandle texture) const;
    // Null if the handle is stale.
    const BufferDesc* buffer_desc(BufferHandle buffer) const;

    // The GPU address shaders reach the buffer by (a pointer in Slang). Every buffer with a
    // shader-visible usage (vertex, index, uniform, storage, indirect) has one.
    uint64_t buffer_address(BufferHandle buffer) const;

    // The persistent mapping of an Upload or Readback buffer; empty for GpuOnly buffers.
    std::span<std::byte> mapped(BufferHandle buffer);
    // Makes CPU writes to an Upload buffer visible to the GPU (a no-op on coherent memory).
    void flush_mapped(BufferHandle buffer, uint64_t offset = 0, uint64_t size = UINT64_MAX);
    // Makes GPU writes to a Readback buffer visible to the CPU (a no-op on coherent memory).
    // Call it after the frame that wrote the data is complete (wait_idle, or kFramesInFlight frames later).
    void invalidate_mapped(BufferHandle buffer, uint64_t offset = 0, uint64_t size = UINT64_MAX);

    // --- Frames ---

    // Waits until the GPU has finished the frame kFramesInFlight frames ago, then recycles its
    // command lists and frees the resources destroyed during it.
    void begin_frame();

    // The swapchain texture to draw into this frame, in Undefined state; hand it over in Present
    // state. Invalid when there is nothing to present to (headless, minimized, zero-sized window).
    // Recreates the swapchain when the window was resized or the old one became unusable.
    // Call it at most once per frame, as late as possible.
    TextureHandle acquire_swapchain_texture();

    // A new command list for this frame, on the graphics queue or, when
    // Capabilities::transfer_queue, the transfer queue (copies and barriers only). Async compute
    // lists arrive with the render graph.
    //
    // Transfer lists of frame N run alongside the GPU's work on frame N-1, and all of frame N's
    // graphics work waits for them. A transfer list that overwrites something frame N-1 (or
    // earlier) may still read starts AfterPreviousFrame instead. FrameStart graphics lists are
    // submitted before the frame's other graphics lists, wherever they were begun.
    CommandList begin_command_list(QueueType queue = QueueType::Graphics, ListStart start = ListStart::InOrder);

    // Submits this frame's command lists, per queue in the order they were begun (FrameStart lists
    // first; transfer lists before graphics), then presents the swapchain texture if one was acquired.
    void end_frame();

    // Waits for all GPU work to finish and frees every pending destroy. For shutdown, tests and tools.
    void wait_idle();

    // Frames begun so far: the current frame between begin_frame() and end_frame().
    uint64_t frame_index() const;

    // The GPU timestamps a frame wrote (CommandList::write_timestamp), by index, in nanoseconds
    // after the frame's first one (index 0 is 0; a counter that wraps is handled): those of the
    // frame whose slot the last begin_frame() reused (kFramesInFlight frames before the current
    // one), once the GPU had finished it. frame is 0 and ns empty
    // before any, or when it wrote none. Valid until the next begin_frame().
    struct FrameTimestamps {
        uint64_t frame = 0;
        std::span<const uint64_t> ns;
    };
    FrameTimestamps completed_timestamps() const;
    // The last frame the GPU has finished (0 before any): resources a frame used are free once
    // completed_frame() reaches it.
    uint64_t completed_frame() const;
    bool in_frame() const;

    // Takes effect at the next swapchain acquire.
    void set_vsync(bool vsync);
    // Whether frames present on the display's refresh now: what the swapchain does, which may not be what
    // was asked (the surface decides); what was asked until there's a swapchain.
    bool vsync() const;

    Format swapchain_format() const; // Unknown when headless
    uint32_t swapchain_width() const;
    uint32_t swapchain_height() const;

private:
    explicit Device(DeviceImpl* impl) : impl_(impl) {}

    DeviceImpl* impl_ = nullptr;
};

} // namespace de::rhi
