#pragma once

#include "core/inline_array.h"
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

    // Resource state transitions, recorded as one barrier command. Batch them: one call with many
    // barriers is cheaper than many calls (BarrierBatch collects them).
    void barriers(std::span<const TextureBarrier> textures, std::span<const BufferBarrier> buffers = {},
                  std::span<const GlobalBarrier> globals = {});
    void barrier(const TextureBarrier& barrier);
    void barrier(const BufferBarrier& barrier);
    void barrier(const GlobalBarrier& barrier);
    // Whole resource, shader states in every shader stage.
    void barrier(TextureHandle texture, ResourceState before, ResourceState after);
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

// Collects barriers and records them as one command. Kept on the stack: the first kInlineBarriers
// of each kind stay inside the object (as in the barrier command itself); global barriers merge
// into one. Each resource range appears at most once per batch: barriers within one command are
// unordered, so two transitions of the same texture can't share a batch.
//     BarrierBatch batch;
//     batch.add({.texture = gbuffer, .before = RenderTarget, .after = ShaderRead, .after_stages = Fragment});
//     batch.add({.buffer = instances, .before = ShaderWrite, .after = IndirectArgument});
//     batch.flush(list);
class BarrierBatch {
public:
    BarrierBatch() = default;
    BarrierBatch(const BarrierBatch&) = delete;
    BarrierBatch& operator=(const BarrierBatch&) = delete;
    BarrierBatch(BarrierBatch&&) = delete;
    BarrierBatch& operator=(BarrierBatch&&) = delete;
    ~BarrierBatch() { DE_ASSERT_MSG(empty(), "BarrierBatch destroyed with barriers not flushed"); }

    BarrierBatch& add(const TextureBarrier& barrier) {
        textures_.push(barrier);
        return *this;
    }
    BarrierBatch& add(const BufferBarrier& barrier) {
        buffers_.push(barrier);
        return *this;
    }
    // Merged with any global barrier already added: the union of the states on each side, and of
    // the shader stages only where a side has shader states (an empty side adds no stages).
    BarrierBatch& add(const GlobalBarrier& barrier) {
        global_.before |= barrier.before;
        global_.after |= barrier.after;
        if (core::has_any(barrier.before, kShaderStates)) {
            global_.before_stages |= barrier.before_stages;
        }
        if (core::has_any(barrier.after, kShaderStates)) {
            global_.after_stages |= barrier.after_stages;
        }
        has_global_ = true;
        return *this;
    }

    [[nodiscard]] bool empty() const { return textures_.empty() && buffers_.empty() && !has_global_; }
    [[nodiscard]] size_t texture_count() const { return textures_.size(); }
    [[nodiscard]] size_t buffer_count() const { return buffers_.size(); }

    // The merged global barrier, or null if none was added.
    const GlobalBarrier* global() const { return has_global_ ? &global_ : nullptr; }

    // Records everything as one barrier command and empties the batch. Does nothing when empty.
    void flush(CommandList& list) {
        if (empty()) {
            return;
        }
        list.barriers(textures_.span(), buffers_.span(),
                      has_global_ ? std::span<const GlobalBarrier>(&global_, 1) : std::span<const GlobalBarrier>());
        clear();
    }

    // Drops every barrier without recording it (a pass that turned out not to run).
    void clear() {
        textures_.clear();
        buffers_.clear();
        global_ = kEmptyGlobal;
        has_global_ = false;
    }

private:
    // Stages start empty and grow only with sides that have shader states.
    static constexpr GlobalBarrier kEmptyGlobal{.before_stages = ShaderStages::None,
                                                .after_stages = ShaderStages::None};

    core::InlineArray<TextureBarrier, kInlineBarriers> textures_{core::heap_allocator(core::MemoryTag::Render)};
    core::InlineArray<BufferBarrier, kInlineBarriers> buffers_{core::heap_allocator(core::MemoryTag::Render)};
    GlobalBarrier global_ = kEmptyGlobal;
    bool has_global_ = false;
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
