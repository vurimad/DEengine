#include "render/graph/render_graph.h"

#include "core/allocator.h"
#include "core/arena.h"
#include "core/array.h"
#include "core/hash.h"
#include "core/hash_map.h"
#include "core/log.h"
#include "core/profile.h"
#include "render/rhi/device.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string_view>
#include <utility>

namespace de::render {

using rhi::ResourceState;
using rhi::ShaderStages;

namespace {

core::Allocator& render_heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

constexpr uint32_t kNone = UINT32_MAX;

// One subresource's state as the graph tracks it: the state and shader stages of its last uses
// (reads in one layout accumulate), and whether its contents may be dropped at the next barrier.
struct SubState {
    ResourceState state = ResourceState::Undefined;
    ShaderStages stages = ShaderStages::None;
    bool discard = false;

    bool operator==(const SubState&) const = default;
};

using ExecuteFn = void (*)(const void* closure, PassContext& context);

struct Use {
    uint32_t resource = 0;
    bool texture = true;
    ResourceState state = ResourceState::Undefined;
    ShaderStages stages = ShaderStages::All;
    rhi::TextureRange range{}; // resolved: no kAllMips / kAllLayers
};

struct Pass {
    const char* name = "";
    uint32_t first_use = 0;
    uint32_t use_count = 0;
    ExecuteFn execute = nullptr;
    const void* closure = nullptr;
    bool never_cull = false;
    bool live = true;
};

struct TextureResource {
    const char* name = "";
    rhi::TextureDesc desc{}; // resolved size; usage gathered from the uses
    float reference_scale = 0.0f;
    bool imported = false;
    rhi::TextureHandle handle{};
    ResourceState final_state = ResourceState::Undefined;
    ShaderStages final_stages = ShaderStages::All;
    uint32_t pool_entry = kNone;
    uint32_t first_state = kNone; // its subresources in RenderGraphState::states, mip-major
    bool needed = false;
};

struct BufferResource {
    const char* name = "";
    rhi::BufferDesc desc{};
    bool imported = false;
    rhi::BufferHandle handle{};
    ResourceState final_state = ResourceState::Undefined;
    ShaderStages final_stages = ShaderStages::All;
    uint32_t pool_entry = kNone;
    SubState state{};
    bool realized = false;
    bool needed = false;
};

// A transient resource kept across frames, with the state its last frame left it in (the next
// frame's first barrier waits for those uses).
struct TexturePoolEntry {
    rhi::TextureDesc desc{};
    rhi::TextureHandle handle{};
    uint64_t last_frame = 0;
    bool taken = false;
    uint32_t next = kNone; // the next entry with the same description hash
    core::Array<SubState> states{render_heap()};
};

struct BufferPoolEntry {
    rhi::BufferDesc desc{};
    rhi::BufferHandle handle{};
    uint64_t last_frame = 0;
    bool taken = false;
    uint32_t next = kNone;
    SubState state{};
};

bool same_desc(const rhi::TextureDesc& a, const rhi::TextureDesc& b) {
    return a.width == b.width && a.height == b.height && a.depth == b.depth && a.mip_count == b.mip_count &&
           a.layer_count == b.layer_count && a.format == b.format && a.usage == b.usage && a.dimension == b.dimension;
}

uint64_t desc_hash(const rhi::TextureDesc& desc) {
    uint64_t hash = core::mix64((uint64_t{desc.width} << 32U) | desc.height);
    hash = core::mix64(hash ^ ((uint64_t{desc.depth} << 32U) | (uint64_t{desc.mip_count} << 16U) | desc.layer_count));
    return core::mix64(hash ^ ((static_cast<uint64_t>(desc.format) << 16U) | (static_cast<uint64_t>(desc.usage) << 8U) |
                               static_cast<uint64_t>(desc.dimension)));
}

bool same_desc(const rhi::BufferDesc& a, const rhi::BufferDesc& b) {
    return a.size == b.size && a.usage == b.usage && a.memory == b.memory;
}

uint64_t desc_hash(const rhi::BufferDesc& desc) {
    return core::mix64(desc.size ^ (static_cast<uint64_t>(desc.usage) << 56U) ^
                       (static_cast<uint64_t>(desc.memory) << 48U)) |
           1U; // never 0
}

rhi::TextureUsage texture_usage_for(ResourceState state) {
    rhi::TextureUsage usage = rhi::TextureUsage::None;
    const auto add = [&](ResourceState bits, rhi::TextureUsage flag) {
        if (core::has_any(state, bits)) {
            usage |= flag;
        }
    };
    add(ResourceState::RenderTarget, rhi::TextureUsage::RenderTarget);
    add(ResourceState::DepthWrite | ResourceState::DepthRead, rhi::TextureUsage::DepthStencil);
    add(ResourceState::ShaderRead, rhi::TextureUsage::Sampled);
    add(ResourceState::ShaderWrite | ResourceState::General, rhi::TextureUsage::Storage);
    add(ResourceState::CopySrc, rhi::TextureUsage::CopySrc);
    add(ResourceState::CopyDst, rhi::TextureUsage::CopyDst);
    return usage;
}

rhi::BufferUsage buffer_usage_for(ResourceState state) {
    rhi::BufferUsage usage = rhi::BufferUsage::None;
    const auto add = [&](ResourceState bits, rhi::BufferUsage flag) {
        if (core::has_any(state, bits)) {
            usage |= flag;
        }
    };
    add(ResourceState::ShaderRead | ResourceState::ShaderWrite | ResourceState::General, rhi::BufferUsage::Storage);
    add(ResourceState::UniformBuffer, rhi::BufferUsage::Uniform);
    add(ResourceState::VertexBuffer, rhi::BufferUsage::Vertex);
    add(ResourceState::IndexBuffer, rhi::BufferUsage::Index);
    add(ResourceState::IndirectArgument, rhi::BufferUsage::Indirect);
    add(ResourceState::CopySrc, rhi::BufferUsage::CopySrc);
    add(ResourceState::CopyDst, rhi::BufferUsage::CopyDst);
    return usage;
}

bool stages_named(ResourceState state, ShaderStages stages) {
    return !core::has_any(state, rhi::kShaderStates) || stages != ShaderStages::None;
}

bool ranges_overlap(const rhi::TextureRange& a, const rhi::TextureRange& b) {
    return a.first_mip < b.first_mip + b.mip_count && b.first_mip < a.first_mip + a.mip_count &&
           a.first_layer < b.first_layer + b.layer_count && b.first_layer < a.first_layer + a.layer_count;
}

// What a subresource in state `from` needs to be used in state `to` for `stages`: a barrier or not,
// and the state it is tracked in afterwards.
//   - A read already covered (its state and shader stages are among the tracked reads) needs none.
//   - A read that shares a layout with the tracked reads gets a barrier for its own stages (chained
//     to the barrier that made the last write visible), and joins them, so the next writer waits
//     for all of them.
//   - Anything else (a write, a first use, a change of layout) gets a barrier and replaces the state.
struct Transition {
    bool barrier = false;
    SubState next;
};

Transition transition(const SubState& from, ResourceState to, ShaderStages stages, bool texture) {
    const bool reads = from.state != ResourceState::Undefined && !core::has_any(from.state, rhi::kWriteStates) &&
                       !core::has_any(to, rhi::kWriteStates);
    if (reads) {
        const bool stages_covered = !core::has_any(to, rhi::kShaderStates) || core::has_all(from.stages, stages);
        if (core::has_all(from.state, to) && stages_covered && !from.discard) {
            return {.barrier = false, .next = from};
        }
        const ResourceState joined = from.state | to;
        if (rhi::is_valid_state(joined, texture)) {
            return {.barrier = true, .next = {.state = joined, .stages = from.stages | stages}};
        }
    }
    return {.barrier = true, .next = {.state = to, .stages = stages}};
}

} // namespace

struct RenderGraphState {
    rhi::Device* device = nullptr;
    RenderGraphDesc desc;
    core::Arena arena{render_heap()}; // this frame's closures and names

    core::Array<Pass> passes{render_heap()};
    core::Array<Use> uses{render_heap()};
    core::Array<TextureResource> textures{render_heap()};
    core::Array<BufferResource> buffers{render_heap()};
    core::Array<SubState> states{render_heap()}; // this frame's subresource states

    core::Array<TexturePoolEntry> texture_pool{render_heap()};
    core::Array<BufferPoolEntry> buffer_pool{render_heap()};
    core::HashMap<uint64_t, uint32_t> texture_buckets{render_heap()}; // description hash -> first entry
    core::HashMap<uint64_t, uint32_t> buffer_buckets{render_heap()};

    rhi::CommandList list;
    bool in_frame = false;
    uint64_t frame = 0;
    uint32_t reference_width = 0;
    uint32_t reference_height = 0;
    uint32_t open_pass = kNone; // in add_pass, between begin_pass and end_pass
    uint32_t barriers = 0;
    uint32_t pool_creations = 0;

    const char* copy_name(const char* name) {
        const std::string_view text = name != nullptr ? std::string_view(name) : std::string_view();
        auto* copy = static_cast<char*>(arena.allocate(text.size() + 1, 1));
        text.copy(copy, text.size());
        copy[text.size()] = '\0';
        return copy;
    }

    uint32_t subresource_count(const rhi::TextureDesc& texture) const {
        return texture.mip_count * texture.layer_count;
    }

    // --- Pools ---

    void rebuild_buckets() {
        texture_buckets.clear();
        for (uint32_t i = 0; i < texture_pool.size(); ++i) {
            const uint64_t hash = desc_hash(texture_pool[i].desc);
            uint32_t* head = texture_buckets.find(hash);
            texture_pool[i].next = head != nullptr ? *head : kNone;
            texture_buckets.set(hash, i);
        }
        buffer_buckets.clear();
        for (uint32_t i = 0; i < buffer_pool.size(); ++i) {
            const uint64_t hash = desc_hash(buffer_pool[i].desc);
            uint32_t* head = buffer_buckets.find(hash);
            buffer_pool[i].next = head != nullptr ? *head : kNone;
            buffer_buckets.set(hash, i);
        }
    }

    // Gives back to the device what no frame has used for a while.
    void evict() {
        bool evicted = false;
        for (size_t i = texture_pool.size(); i-- > 0;) {
            if (frame - texture_pool[i].last_frame > desc.keep_unused_frames) {
                device->destroy(texture_pool[i].handle);
                if (i + 1 != texture_pool.size()) {
                    texture_pool[i] = std::move(texture_pool[texture_pool.size() - 1]);
                }
                texture_pool.pop();
                evicted = true;
            }
        }
        for (size_t i = buffer_pool.size(); i-- > 0;) {
            if (frame - buffer_pool[i].last_frame > desc.keep_unused_frames) {
                device->destroy(buffer_pool[i].handle);
                buffer_pool[i] = buffer_pool[buffer_pool.size() - 1];
                buffer_pool.pop();
                evicted = true;
            }
        }
        if (evicted) {
            rebuild_buckets();
        }
    }

    uint32_t acquire_texture(const rhi::TextureDesc& wanted, const char* name) {
        const uint64_t hash = desc_hash(wanted);
        if (const uint32_t* head = texture_buckets.find(hash)) {
            for (uint32_t i = *head; i != kNone; i = texture_pool[i].next) {
                if (!texture_pool[i].taken && same_desc(texture_pool[i].desc, wanted)) {
                    texture_pool[i].taken = true;
                    texture_pool[i].last_frame = frame;
                    return i;
                }
            }
        }
        rhi::TextureDesc create = wanted;
        create.debug_name = name;
        const auto texture = device->create_texture(create);
        DE_VERIFY_MSG(texture.ok(), "the render graph couldn't create a transient texture");
        TexturePoolEntry entry{.desc = wanted, .handle = *texture, .last_frame = frame, .taken = true};
        entry.states.resize(subresource_count(wanted));
        const uint32_t* head = texture_buckets.find(hash);
        entry.next = head != nullptr ? *head : kNone;
        texture_pool.push(std::move(entry));
        const auto index = static_cast<uint32_t>(texture_pool.size() - 1);
        texture_buckets.set(hash, index);
        ++pool_creations;
        return index;
    }

    uint32_t acquire_buffer(const rhi::BufferDesc& wanted, const char* name) {
        const uint64_t hash = desc_hash(wanted);
        if (const uint32_t* head = buffer_buckets.find(hash)) {
            for (uint32_t i = *head; i != kNone; i = buffer_pool[i].next) {
                if (!buffer_pool[i].taken && same_desc(buffer_pool[i].desc, wanted)) {
                    buffer_pool[i].taken = true;
                    buffer_pool[i].last_frame = frame;
                    return i;
                }
            }
        }
        rhi::BufferDesc create = wanted;
        create.debug_name = name;
        const auto buffer = device->create_buffer(create);
        DE_VERIFY_MSG(buffer.ok(), "the render graph couldn't create a transient buffer");
        const uint32_t* head = buffer_buckets.find(hash);
        buffer_pool.push(BufferPoolEntry{.desc = wanted,
                                         .handle = *buffer,
                                         .last_frame = frame,
                                         .taken = true,
                                         .next = head != nullptr ? *head : kNone});
        const auto index = static_cast<uint32_t>(buffer_pool.size() - 1);
        buffer_buckets.set(hash, index);
        ++pool_creations;
        return index;
    }

    // --- Resources ---

    // A transient texture gets its texture from the pool, sized and with every usage its passes
    // declared (in immediate mode, every usage its format allows: later uses aren't known yet).
    void realize(TextureResource& texture) {
        if (texture.imported || texture.handle.is_valid()) {
            return;
        }
        if (desc.immediate) {
            texture.desc.usage |= broad_usage(texture.desc.format);
        }
        texture.pool_entry = acquire_texture(texture.desc, texture.name);
        const TexturePoolEntry& entry = texture_pool[texture.pool_entry];
        texture.handle = entry.handle;
        texture.first_state = static_cast<uint32_t>(states.size());
        for (const SubState& previous : entry.states) {
            // Last frame's uses are waited for; their contents aren't kept.
            states.push(SubState{.state = previous.state, .stages = previous.stages, .discard = true});
        }
    }

    void realize(BufferResource& buffer) {
        if (buffer.imported || buffer.realized) {
            return;
        }
        if (desc.immediate) {
            buffer.desc.usage |= rhi::BufferUsage::Storage | rhi::BufferUsage::CopySrc | rhi::BufferUsage::CopyDst;
        }
        buffer.pool_entry = acquire_buffer(buffer.desc, buffer.name);
        buffer.handle = buffer_pool[buffer.pool_entry].handle;
        buffer.state = buffer_pool[buffer.pool_entry].state;
        buffer.realized = true;
    }

    rhi::TextureUsage broad_usage(rhi::Format format) const {
        const rhi::Capabilities& caps = device->capabilities();
        rhi::TextureUsage usage = rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc | rhi::TextureUsage::CopyDst;
        if (rhi::format_info(format).depth) {
            return usage | rhi::TextureUsage::DepthStencil;
        }
        if (caps.supports(format, rhi::FormatSupport::RenderTarget)) {
            usage |= rhi::TextureUsage::RenderTarget;
        }
        if (caps.supports(rhi::storage_format(format), rhi::FormatSupport::Storage)) {
            usage |= rhi::TextureUsage::Storage;
        }
        return usage;
    }

    // --- Barriers ---

    void log_barrier(const char* pass, const char* resource, const SubState& from, ResourceState to,
                     const rhi::TextureRange* range) const {
        if (!desc.log_transitions) {
            return;
        }
        if (range != nullptr) {
            DE_LOG_INFO("render", "graph: %s: %s mips %u+%u layers %u+%u: %s -> %s%s", pass, resource, range->first_mip,
                        range->mip_count, range->first_layer, range->layer_count,
                        rhi::resource_state_name(from.state).c_str(), rhi::resource_state_name(to).c_str(),
                        from.discard ? " (discard)" : "");
        } else {
            DE_LOG_INFO("render", "graph: %s: %s: %s -> %s", pass, resource,
                        rhi::resource_state_name(from.state).c_str(), rhi::resource_state_name(to).c_str());
        }
    }

    // Barriers that bring a texture's subresources in range to `to`, one per run of layers in the
    // same state within a mip, or one for the whole range when it is all in one state.
    void transition_texture(const char* pass, TextureResource& texture, const rhi::TextureRange& range,
                            ResourceState to, ShaderStages stages, rhi::BarrierBatch& batch) {
        SubState* subresources = states.data() + texture.first_state;
        const uint32_t layers = texture.desc.layer_count;
        const auto at = [&](uint32_t mip, uint32_t layer) -> SubState& { return subresources[(mip * layers) + layer]; };
        const auto emit = [&](const SubState& from, const rhi::TextureRange& part) {
            const Transition step = transition(from, to, stages, true);
            if (step.barrier) {
                batch.add(rhi::TextureBarrier{.texture = texture.handle,
                                              .before = from.state,
                                              .after = to,
                                              .before_stages =
                                                  from.stages != ShaderStages::None ? from.stages : ShaderStages::All,
                                              .after_stages = stages,
                                              .range = part,
                                              .discard = from.discard});
                ++barriers;
                log_barrier(pass, texture.name, from, to, &part);
            }
            for (uint32_t mip = part.first_mip; mip < part.first_mip + part.mip_count; ++mip) {
                for (uint32_t layer = part.first_layer; layer < part.first_layer + part.layer_count; ++layer) {
                    at(mip, layer) = step.next;
                }
            }
        };

        const SubState first = at(range.first_mip, range.first_layer);
        bool uniform = true;
        for (uint32_t mip = range.first_mip; mip < range.first_mip + range.mip_count && uniform; ++mip) {
            for (uint32_t layer = range.first_layer; layer < range.first_layer + range.layer_count; ++layer) {
                uniform = uniform && at(mip, layer) == first;
            }
        }
        if (uniform) {
            emit(first, range);
            return;
        }
        for (uint32_t mip = range.first_mip; mip < range.first_mip + range.mip_count; ++mip) {
            uint32_t run = range.first_layer;
            for (uint32_t layer = run + 1; layer <= range.first_layer + range.layer_count; ++layer) {
                if (layer == range.first_layer + range.layer_count || !(at(mip, layer) == at(mip, run))) {
                    const SubState from = at(mip, run);
                    emit(from, {.first_mip = mip, .mip_count = 1, .first_layer = run, .layer_count = layer - run});
                    run = layer;
                }
            }
        }
    }

    void transition_buffer(const char* pass, BufferResource& buffer, ResourceState to, ShaderStages stages,
                           rhi::BarrierBatch& batch) {
        const Transition step = transition(buffer.state, to, stages, false);
        if (step.barrier) {
            batch.add(rhi::BufferBarrier{
                .buffer = buffer.handle,
                .before = buffer.state.state,
                .after = to,
                .before_stages = buffer.state.stages != ShaderStages::None ? buffer.state.stages : ShaderStages::All,
                .after_stages = stages});
            ++barriers;
            log_barrier(pass, buffer.name, buffer.state, to, nullptr);
        }
        buffer.state = step.next;
    }

    // --- Passes ---

    void run(uint32_t pass_index) {
        const Pass& pass = passes[pass_index];
        rhi::BarrierBatch batch;
        for (uint32_t i = pass.first_use; i < pass.first_use + pass.use_count; ++i) {
            const Use& use = uses[i];
            if (use.texture) {
                TextureResource& texture = textures[use.resource];
                realize(texture);
                transition_texture(pass.name, texture, use.range, use.state, use.stages, batch);
            } else {
                BufferResource& buffer = buffers[use.resource];
                realize(buffer);
                transition_buffer(pass.name, buffer, use.state, use.stages, batch);
            }
        }
        const rhi::ScopedDebugLabel label(list, pass.name);
        batch.flush(list);
        PassContext context(list, *device, *this, pass_index);
        pass.execute(pass.closure, context);
    }

    // Keeps the passes whose writes something needs: an imported resource, or a later kept pass's
    // use. Earlier writers of a needed resource are kept too (a later pass may load what they
    // wrote).
    void cull() {
        for (TextureResource& texture : textures) {
            texture.needed = texture.imported;
        }
        for (BufferResource& buffer : buffers) {
            buffer.needed = buffer.imported;
        }
        for (size_t p = passes.size(); p-- > 0;) {
            Pass& pass = passes[p];
            bool live = pass.never_cull;
            for (uint32_t i = pass.first_use; i < pass.first_use + pass.use_count && !live; ++i) {
                const Use& use = uses[i];
                const bool needed = use.texture ? textures[use.resource].needed : buffers[use.resource].needed;
                live = needed && core::has_any(use.state, rhi::kWriteStates);
            }
            pass.live = live;
            if (live) {
                for (uint32_t i = pass.first_use; i < pass.first_use + pass.use_count; ++i) {
                    const Use& use = uses[i];
                    (use.texture ? textures[use.resource].needed : buffers[use.resource].needed) = true;
                }
            }
        }
    }

    // Imported resources to the states they must leave in; transients' states back to the pool.
    void finish(rhi::BarrierBatch& batch) {
        for (TextureResource& texture : textures) {
            if (texture.first_state == kNone) {
                continue; // never used
            }
            if (texture.imported) {
                if (texture.final_state != ResourceState::Undefined) {
                    transition_texture("graph end", texture,
                                       {.first_mip = 0,
                                        .mip_count = texture.desc.mip_count,
                                        .first_layer = 0,
                                        .layer_count = texture.desc.layer_count},
                                       texture.final_state, texture.final_stages, batch);
                }
            } else if (texture.pool_entry != kNone) {
                TexturePoolEntry& entry = texture_pool[texture.pool_entry];
                for (uint32_t i = 0; i < entry.states.size(); ++i) {
                    entry.states[i] = states[texture.first_state + i];
                    entry.states[i].discard = false;
                }
            }
        }
        for (BufferResource& buffer : buffers) {
            if (buffer.imported) {
                if (buffer.final_state != ResourceState::Undefined) {
                    transition_buffer("graph end", buffer, buffer.final_state, buffer.final_stages, batch);
                }
            } else if (buffer.realized) {
                buffer_pool[buffer.pool_entry].state = buffer.state;
            }
        }
    }
};

// --- PassBuilder ---

GraphTexture PassBuilder::create_texture(const GraphTextureDesc& desc) {
    RenderGraphState& state = *state_;
    TextureResource texture{
        .name = state.copy_name(desc.desc.debug_name), .desc = desc.desc, .reference_scale = desc.reference_scale};
    texture.desc.debug_name = nullptr;
    if (desc.reference_scale > 0.0f) {
        const auto scaled = [&](uint32_t size) {
            return std::max(1U, static_cast<uint32_t>(std::lround(static_cast<float>(size) * desc.reference_scale)));
        };
        texture.desc.width = scaled(state.reference_width);
        texture.desc.height = scaled(state.reference_height);
    }
    DE_ASSERT_MSG(texture.desc.format != rhi::Format::Unknown && texture.desc.width > 0 && texture.desc.height > 0 &&
                      texture.desc.mip_count > 0 && texture.desc.layer_count > 0,
                  "a transient texture needs a format and sizes");
    state.textures.push(texture);
    return GraphTexture{static_cast<uint32_t>(state.textures.size() - 1)};
}

GraphBuffer PassBuilder::create_buffer(const rhi::BufferDesc& desc) {
    RenderGraphState& state = *state_;
    DE_ASSERT_MSG(desc.size > 0, "a transient buffer needs a size");
    BufferResource buffer{.name = state.copy_name(desc.debug_name), .desc = desc};
    buffer.desc.debug_name = nullptr;
    state.buffers.push(buffer);
    return GraphBuffer{static_cast<uint32_t>(state.buffers.size() - 1)};
}

PassBuilder& PassBuilder::use(GraphTexture texture, ResourceState state_used, ShaderStages stages,
                              const rhi::TextureRange& range) {
    RenderGraphState& state = *state_;
    DE_ASSERT_MSG(texture.is_valid() && texture.index < state.textures.size(), "a stale or invalid graph texture");
    DE_ASSERT_MSG(state_used != ResourceState::Undefined && rhi::is_valid_state(state_used, true) &&
                      stages_named(state_used, stages),
                  "a texture use needs a valid state (with shader stages for shader states)");
    TextureResource& resource = state.textures[texture.index];
    DE_ASSERT_MSG(range.first_mip < resource.desc.mip_count && range.first_layer < resource.desc.layer_count,
                  "a texture use outside the texture");
    const rhi::TextureRange resolved{
        .first_mip = range.first_mip,
        .mip_count = range.mip_count == rhi::kAllMips ? resource.desc.mip_count - range.first_mip : range.mip_count,
        .first_layer = range.first_layer,
        .layer_count =
            range.layer_count == rhi::kAllLayers ? resource.desc.layer_count - range.first_layer : range.layer_count};
    DE_ASSERT_MSG(resolved.mip_count > 0 && resolved.first_mip + resolved.mip_count <= resource.desc.mip_count &&
                      resolved.layer_count > 0 &&
                      resolved.first_layer + resolved.layer_count <= resource.desc.layer_count,
                  "a texture use outside the texture");
    Pass& pass = state.passes[pass_];
    for (uint32_t i = pass.first_use; i < pass.first_use + pass.use_count; ++i) {
        const Use& other = state.uses[i];
        DE_ASSERT_MSG(!other.texture || other.resource != texture.index || !ranges_overlap(other.range, resolved),
                      "one pass uses the same subresources twice: declare them once, in one state");
        static_cast<void>(other);
    }
    if (!resource.imported) {
        resource.desc.usage |= texture_usage_for(state_used);
    }
    state.uses.push(
        Use{.resource = texture.index, .texture = true, .state = state_used, .stages = stages, .range = resolved});
    ++pass.use_count;
    return *this;
}

PassBuilder& PassBuilder::use(GraphBuffer buffer, ResourceState state_used, ShaderStages stages) {
    RenderGraphState& state = *state_;
    DE_ASSERT_MSG(buffer.is_valid() && buffer.index < state.buffers.size(), "a stale or invalid graph buffer");
    DE_ASSERT_MSG(state_used != ResourceState::Undefined && rhi::is_valid_state(state_used, false) &&
                      stages_named(state_used, stages),
                  "a buffer use needs a valid state (with shader stages for shader states)");
    Pass& pass = state.passes[pass_];
    for (uint32_t i = pass.first_use; i < pass.first_use + pass.use_count; ++i) {
        DE_ASSERT_MSG(state.uses[i].texture || state.uses[i].resource != buffer.index,
                      "one pass uses the same buffer twice: declare it once, in one state");
    }
    BufferResource& resource = state.buffers[buffer.index];
    if (!resource.imported) {
        resource.desc.usage |= buffer_usage_for(state_used);
    }
    state.uses.push(Use{.resource = buffer.index, .texture = false, .state = state_used, .stages = stages});
    ++pass.use_count;
    return *this;
}

PassBuilder& PassBuilder::never_cull() {
    state_->passes[pass_].never_cull = true;
    return *this;
}

// --- PassContext ---

namespace {

bool declared(const RenderGraphState& state, uint32_t pass_index, uint32_t resource, bool texture) {
    const Pass& pass = state.passes[pass_index];
    for (uint32_t i = pass.first_use; i < pass.first_use + pass.use_count; ++i) {
        if (state.uses[i].texture == texture && state.uses[i].resource == resource) {
            return true;
        }
    }
    return false;
}

} // namespace

rhi::TextureHandle PassContext::texture(GraphTexture texture) const {
    DE_ASSERT_MSG(texture.is_valid() && texture.index < state_.textures.size() &&
                      declared(state_, pass_, texture.index, true),
                  "a pass reached a texture it didn't declare in its setup");
    return state_.textures[texture.index].handle;
}

rhi::BufferHandle PassContext::buffer(GraphBuffer buffer) const {
    DE_ASSERT_MSG(buffer.is_valid() && buffer.index < state_.buffers.size() &&
                      declared(state_, pass_, buffer.index, false),
                  "a pass reached a buffer it didn't declare in its setup");
    return state_.buffers[buffer.index].handle;
}

const rhi::TextureDesc& PassContext::desc(GraphTexture texture) const {
    DE_ASSERT_MSG(texture.is_valid() && texture.index < state_.textures.size(), "a stale or invalid graph texture");
    return state_.textures[texture.index].desc;
}

// --- RenderGraph ---

core::Result<RenderGraph> RenderGraph::create(rhi::Device& device, const RenderGraphDesc& desc) {
    if (desc.immediate && DE_BUILD_DEV == 0) {
        return core::fail(core::ErrorCode::Unsupported, "the render graph's immediate mode is for dev builds");
    }
    auto* state =
        ::new (render_heap().allocate(sizeof(RenderGraphState), alignof(RenderGraphState))) RenderGraphState();
    state->device = &device;
    state->desc = desc;
    return RenderGraph(state);
}

RenderGraph::~RenderGraph() {
    if (state_ == nullptr) {
        return;
    }
    DE_ASSERT_MSG(!state_->in_frame, "render graph destroyed between begin() and execute()");
    for (const TexturePoolEntry& entry : state_->texture_pool) {
        state_->device->destroy(entry.handle);
    }
    for (const BufferPoolEntry& entry : state_->buffer_pool) {
        state_->device->destroy(entry.handle);
    }
    state_->~RenderGraphState();
    render_heap().deallocate(state_, sizeof(RenderGraphState), alignof(RenderGraphState));
}

RenderGraph::RenderGraph(RenderGraph&& other) noexcept : state_(std::exchange(other.state_, nullptr)) {}

RenderGraph& RenderGraph::operator=(RenderGraph&& other) noexcept {
    if (this != &other) {
        const RenderGraph discarded(std::exchange(state_, std::exchange(other.state_, nullptr)));
    }
    return *this;
}

void RenderGraph::begin(uint32_t reference_width, uint32_t reference_height) {
    DE_PROFILE_ZONE("render graph begin");
    RenderGraphState& state = *state_;
    DE_ASSERT_MSG(state.device->in_frame(), "the render graph runs between device.begin_frame() and end_frame()");
    DE_ASSERT_MSG(!state.in_frame, "render graph begin() twice without execute()");
    ++state.frame;
    state.arena.reset();
    state.passes.clear();
    state.uses.clear();
    state.textures.clear();
    state.buffers.clear();
    state.states.clear();
    state.barriers = 0;
    state.reference_width = reference_width;
    state.reference_height = reference_height;
    state.evict();
    for (TexturePoolEntry& entry : state.texture_pool) {
        entry.taken = false;
    }
    for (BufferPoolEntry& entry : state.buffer_pool) {
        entry.taken = false;
    }
    state.list = state.device->begin_command_list();
    state.in_frame = true;
}

GraphTexture RenderGraph::import_texture(const ImportedTexture& imported) {
    RenderGraphState& state = *state_;
    DE_ASSERT_MSG(state.in_frame, "import between begin() and execute()");
    const rhi::TextureDesc* desc = state.device->texture_desc(imported.texture);
    DE_ASSERT_MSG(desc != nullptr, "importing a stale texture handle");
    TextureResource texture{.name = state.copy_name(imported.name),
                            .desc = desc != nullptr ? *desc : rhi::TextureDesc{},
                            .imported = true,
                            .handle = imported.texture,
                            .final_state = imported.final_state,
                            .final_stages = imported.final_stages};
    texture.first_state = static_cast<uint32_t>(state.states.size());
    for (uint32_t i = 0; i < state.subresource_count(texture.desc); ++i) {
        state.states.push(SubState{.state = imported.state, .stages = imported.stages, .discard = imported.discard});
    }
    state.textures.push(texture);
    return GraphTexture{static_cast<uint32_t>(state.textures.size() - 1)};
}

GraphBuffer RenderGraph::import_buffer(const ImportedBuffer& imported) {
    RenderGraphState& state = *state_;
    DE_ASSERT_MSG(state.in_frame, "import between begin() and execute()");
    const rhi::BufferDesc* desc = state.device->buffer_desc(imported.buffer);
    DE_ASSERT_MSG(desc != nullptr, "importing a stale buffer handle");
    state.buffers.push(BufferResource{.name = state.copy_name(imported.name),
                                      .desc = desc != nullptr ? *desc : rhi::BufferDesc{},
                                      .imported = true,
                                      .handle = imported.buffer,
                                      .final_state = imported.final_state,
                                      .final_stages = imported.final_stages,
                                      .state = {.state = imported.state, .stages = imported.stages},
                                      .realized = true});
    return GraphBuffer{static_cast<uint32_t>(state.buffers.size() - 1)};
}

uint32_t RenderGraph::begin_pass(const char* name) {
    RenderGraphState& state = *state_;
    DE_ASSERT_MSG(state.in_frame, "add_pass between begin() and execute()");
    DE_ASSERT_MSG(state.open_pass == kNone, "add_pass inside another pass's setup");
    state.passes.push(Pass{.name = state.copy_name(name), .first_use = static_cast<uint32_t>(state.uses.size())});
    state.open_pass = static_cast<uint32_t>(state.passes.size() - 1);
    return state.open_pass;
}

void* RenderGraph::allocate_closure(size_t size, size_t alignment) {
    return state_->arena.allocate(std::max<size_t>(size, 1), alignment);
}

void RenderGraph::end_pass(uint32_t pass, ExecuteFn execute, const void* closure) {
    RenderGraphState& state = *state_;
    state.passes[pass].execute = execute;
    state.passes[pass].closure = closure;
    state.open_pass = kNone;
    if (state.desc.immediate) {
        state.run(pass);
    }
}

void RenderGraph::execute() {
    DE_PROFILE_ZONE("render graph execute");
    RenderGraphState& state = *state_;
    DE_ASSERT_MSG(state.in_frame, "render graph execute() without begin()");
    if (!state.desc.immediate) {
        state.cull();
        for (uint32_t p = 0; p < state.passes.size(); ++p) {
            if (state.passes[p].live) {
                state.run(p);
            }
        }
    }
    rhi::BarrierBatch batch;
    state.finish(batch);
    batch.flush(state.list);
    state.in_frame = false;
}

rhi::ResourceState RenderGraph::final_texture_state(GraphTexture texture) const {
    const RenderGraphState& state = *state_;
    DE_ASSERT_MSG(texture.is_valid() && texture.index < state.textures.size(), "a stale or invalid graph texture");
    const TextureResource& resource = state.textures[texture.index];
    return resource.first_state == kNone ? ResourceState::Undefined : state.states[resource.first_state].state;
}

RenderGraphStats RenderGraph::stats() const {
    const RenderGraphState& state = *state_;
    RenderGraphStats stats{
        .passes = static_cast<uint32_t>(state.passes.size()),
        .barriers = state.barriers,
        .pooled_textures = static_cast<uint32_t>(state.texture_pool.size()),
        .pooled_buffers = static_cast<uint32_t>(state.buffer_pool.size()),
        .pool_creations = state.pool_creations,
    };
    for (const Pass& pass : state.passes) {
        stats.culled_passes += pass.live ? 0 : 1;
    }
    return stats;
}

void RenderGraph::dump() const {
    const RenderGraphState& state = *state_;
    DE_LOG_INFO("render", "render graph: %zu passes, %zu textures, %zu buffers, %u barriers", state.passes.size(),
                state.textures.size(), state.buffers.size(), state.barriers);
    for (const Pass& pass : state.passes) {
        DE_LOG_INFO("render", "  pass %s%s", pass.name, pass.live ? "" : " (culled)");
        for (uint32_t i = pass.first_use; i < pass.first_use + pass.use_count; ++i) {
            const Use& use = state.uses[i];
            if (use.texture) {
                const TextureResource& texture = state.textures[use.resource];
                DE_LOG_INFO("render", "    %s %s mips %u+%u layers %u+%u%s",
                            rhi::resource_state_name(use.state).c_str(), texture.name, use.range.first_mip,
                            use.range.mip_count, use.range.first_layer, use.range.layer_count,
                            texture.imported ? " (imported)" : "");
            } else {
                const BufferResource& buffer = state.buffers[use.resource];
                DE_LOG_INFO("render", "    %s %s%s", rhi::resource_state_name(use.state).c_str(), buffer.name,
                            buffer.imported ? " (imported)" : "");
            }
        }
    }
}

} // namespace de::render
