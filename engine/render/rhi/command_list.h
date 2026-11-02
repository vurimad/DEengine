#pragma once

#include "render/rhi/rhi.h"

#include <cstdint>
#include <span>

namespace de::rhi {

struct CommandListImpl;

inline constexpr Color kDefaultLabelColor{.r = 0.6f, .g = 0.6f, .b = 0.6f, .a = 1.0f};

// Records GPU commands for one queue. Get one from Device::begin_command_list(); it is submitted
// by Device::end_frame(), in the order the lists were begun, and is invalid after that.
// A small non-owning view: copy it freely within the frame. Record from one thread at a time.
class CommandList {
public:
    CommandList() = default;

    bool is_valid() const { return impl_ != nullptr; }

    // Resource state transitions. Batch them: one call with many barriers is cheaper than many calls.
    void barriers(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers = {});
    void barrier(TextureHandle texture, ResourceState before, ResourceState after, const TextureRange& range = {});
    void barrier(BufferHandle buffer, ResourceState before, ResourceState after);

    // Dynamic rendering: no render pass or framebuffer objects. Sets the viewport and scissor to the area.
    void begin_rendering(const RenderingDesc& desc);
    void end_rendering();

    void set_viewport(const Viewport& viewport);
    void set_scissor(const Rect& rect);

    // Graphics pipelines bind inside begin_rendering, compute pipelines outside it.
    void bind_pipeline(PipelineHandle pipeline);
    // Push constants for the bound pipeline: offset + data.size() <= kMaxPushConstantBytes,
    // both multiples of 4. They stay set across pipeline binds within the list.
    void push_constants(std::span<const std::byte> data, uint32_t offset = 0);
    template <typename T>
    void push_constants(const T& data, uint32_t offset = 0) {
        static_assert(sizeof(T) <= kMaxPushConstantBytes, "push constants are limited to kMaxPushConstantBytes");
        push_constants(std::as_bytes(std::span(&data, 1)), offset);
    }

    void draw(uint32_t vertex_count, uint32_t instance_count = 1, uint32_t first_vertex = 0,
              uint32_t first_instance = 0);
    // The index buffer must be in IndexBuffer state.
    void bind_index_buffer(BufferHandle buffer, uint64_t offset, IndexType type);
    void draw_indexed(uint32_t index_count, uint32_t instance_count = 1, uint32_t first_index = 0,
                      int32_t vertex_offset = 0, uint32_t first_instance = 0);
    void dispatch(uint32_t groups_x, uint32_t groups_y = 1, uint32_t groups_z = 1);

    // Buffers must be in CopySrc / CopyDst state.
    void copy_buffer(BufferHandle src, uint64_t src_offset, BufferHandle dst, uint64_t dst_offset, uint64_t size);
    // Fills size bytes (a multiple of 4) with a repeated 32-bit value. The buffer must be in CopyDst state.
    void fill_buffer(BufferHandle buffer, uint64_t offset, uint64_t size, uint32_t value);
    void copy_buffer_to_texture(BufferHandle src, TextureHandle dst, const BufferTextureCopy& region = {});
    void copy_texture_to_buffer(TextureHandle src, BufferHandle dst, const BufferTextureCopy& region = {});

    // Named regions, shown in RenderDoc, Nsight and validation messages. Must nest within the list.
    void begin_debug_label(const char* name, Color color = kDefaultLabelColor);
    void end_debug_label();

private:
    friend struct DeviceImpl;
    explicit CommandList(CommandListImpl* impl) : impl_(impl) {}

    CommandListImpl* impl_ = nullptr;
};

// Opens a debug label for the rest of the scope.
class ScopedDebugLabel {
public:
    ScopedDebugLabel(CommandList& list, const char* name, Color color = kDefaultLabelColor) : list_(list) {
        list_.begin_debug_label(name, color);
    }
    ~ScopedDebugLabel() { list_.end_debug_label(); }
    ScopedDebugLabel(const ScopedDebugLabel&) = delete;
    ScopedDebugLabel& operator=(const ScopedDebugLabel&) = delete;
    ScopedDebugLabel(ScopedDebugLabel&&) = delete;
    ScopedDebugLabel& operator=(ScopedDebugLabel&&) = delete;

private:
    CommandList& list_;
};

} // namespace de::rhi
