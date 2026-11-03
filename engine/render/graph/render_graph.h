#pragma once

#include "core/result.h"
#include "render/rhi/command_list.h"
#include "render/rhi/rhi.h"

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

namespace de::rhi {
class Device;
}

namespace de::render {

struct RenderGraphState;

// A texture or buffer of this frame's graph: valid until the graph's next begin().
struct GraphTexture {
    uint32_t index = UINT32_MAX;
    [[nodiscard]] bool is_valid() const { return index != UINT32_MAX; }
};

struct GraphBuffer {
    uint32_t index = UINT32_MAX;
    [[nodiscard]] bool is_valid() const { return index != UINT32_MAX; }
};

// A transient texture: made by the graph for this frame, from a pool reused across frames. Its
// contents are undefined when the frame's first pass that uses it starts. desc.usage may stay
// None: the graph adds every usage the passes' states need.
struct GraphTextureDesc {
    rhi::TextureDesc desc;
    // Above 0: width and height are the graph's reference size (begin()) times this, at least 1;
    // desc.width and desc.height are ignored.
    float reference_scale = 0.0f;
};

// A texture made outside the graph (the swapchain image, persistent textures, history), in the
// state it is in now. The graph leaves it in final_state, or in its last use's state when
// final_state is Undefined (final_texture_state() tells which).
struct ImportedTexture {
    rhi::TextureHandle texture;
    const char* name = nullptr; // for logs; copied
    rhi::ResourceState state = rhi::ResourceState::Undefined;
    rhi::ShaderStages stages = rhi::ShaderStages::All;
    rhi::ResourceState final_state = rhi::ResourceState::Undefined;
    rhi::ShaderStages final_stages = rhi::ShaderStages::All;
    bool discard = false; // its contents aren't needed: the first use doesn't keep them
};

struct ImportedBuffer {
    rhi::BufferHandle buffer;
    const char* name = nullptr;
    rhi::ResourceState state = rhi::ResourceState::Undefined;
    rhi::ShaderStages stages = rhi::ShaderStages::All;
    rhi::ResourceState final_state = rhi::ResourceState::Undefined;
    rhi::ShaderStages final_stages = rhi::ShaderStages::All;
};

// What a pass's setup declares: the resources it creates and every use it makes, each in one
// state. A write state (RenderTarget, DepthWrite, ShaderWrite, CopyDst, General) makes the pass a
// writer of those subresources. Uses of one texture in one pass must not overlap.
class PassBuilder {
public:
    GraphTexture create_texture(const GraphTextureDesc& desc);
    GraphBuffer create_buffer(const rhi::BufferDesc& desc);

    PassBuilder& use(GraphTexture texture, rhi::ResourceState state, rhi::ShaderStages stages = rhi::ShaderStages::All,
                     const rhi::TextureRange& range = {});
    PassBuilder& use(GraphBuffer buffer, rhi::ResourceState state, rhi::ShaderStages stages = rhi::ShaderStages::All);

    // Runs even if nothing reads what it writes (side effects outside the graph: readbacks,
    // queries, debug output).
    PassBuilder& never_cull();

private:
    friend class RenderGraph;
    PassBuilder(RenderGraphState* state, uint32_t pass) : state_(state), pass_(pass) {}

    RenderGraphState* state_;
    uint32_t pass_;
};

// What a pass's execute gets: its command list (graphics), inside a debug label named after the
// pass, with the barriers its uses need already recorded.
class PassContext {
public:
    rhi::CommandList& list;
    rhi::Device& device;

    // The resource behind a graph handle. Only for resources this pass declared (asserted).
    rhi::TextureHandle texture(GraphTexture texture) const;
    rhi::BufferHandle buffer(GraphBuffer buffer) const;
    // The texture's final description: its size after reference scaling, its usage.
    const rhi::TextureDesc& desc(GraphTexture texture) const;

private:
    friend struct RenderGraphState;
    PassContext(rhi::CommandList& command_list, rhi::Device& gpu, const RenderGraphState& state, uint32_t pass)
        : list(command_list), device(gpu), state_(state), pass_(pass) {}

    const RenderGraphState& state_;
    uint32_t pass_;
};

struct RenderGraphDesc {
    // Runs each pass as soon as it is added: no culling, no reordering. For bisecting graph bugs.
    bool immediate = false;
    // Logs every barrier the graph records, with its pass and resource ("render" channel).
    bool log_transitions = false;
    // Transient resources unused for this many frames go back to the device.
    uint32_t keep_unused_frames = 8;
};

struct RenderGraphStats {
    uint32_t passes = 0;        // added in the last frame
    uint32_t culled_passes = 0; // of those, culled
    uint32_t barriers = 0;      // texture and buffer barriers recorded
    uint32_t pooled_textures = 0;
    uint32_t pooled_buffers = 0;
    uint32_t pool_creations = 0; // transient resources created since the graph was, in total
    uint32_t skipped_passes = 0; // in the last frame: a transient they use couldn't be made (logged)
};

// The frame graph (docs/design.md, Rendering: render graph v1). Each frame:
//
//     graph.begin(width, height);                    // after device.begin_frame()
//     GraphTexture back = graph.import_texture({.texture = backbuffer, ..., .final_state = Present});
//     struct BloomData { GraphTexture half; };
//     const BloomData& bloom = graph.add_pass<BloomData>("bloom",
//         [&](PassBuilder& pass, BloomData& data) {
//             data.half = pass.create_texture({.desc = ..., .reference_scale = 0.5f});
//             pass.use(data.half, ResourceState::RenderTarget);
//         },
//         [](const BloomData& data, PassContext& context) { ... context.texture(data.half) ... });
//     graph.add_pass("compose",                      // no data: handles made before it, by value
//         [&](PassBuilder& pass) { pass.use(bloom.half, ...); pass.use(back, ...); },
//         [half = bloom.half, back](PassContext& context) { ... });
//     graph.execute();                               // before device.end_frame()
//
// setup runs inside add_pass; execute runs in execute() (at once in immediate mode), in the order
// the passes were added, unless culled: a pass runs only if it writes something an imported
// resource or a later running pass uses, or never_cull() says so. Every earlier writer of such a
// resource runs too: v1 can't tell a write that overwrites everything from one that loads. The
// graph records the barriers between passes from their declared uses, on its own graphics list.
// Importing one texture or buffer twice in a frame gives the same handle.
//
// A pass's data is where setup leaves the handles it creates for its execute (an execute lambda
// is built before setup runs, so it can't capture them). Data and execute closures live in the
// graph's frame arena and are never destroyed: hold graph handles, pointers and values, never
// containers or references to the stack. Single-threaded, move-only; the device must outlive it.
class RenderGraph {
public:
    static core::Result<RenderGraph> create(rhi::Device& device, const RenderGraphDesc& desc = {});

    ~RenderGraph();
    RenderGraph(RenderGraph&& other) noexcept;
    RenderGraph& operator=(RenderGraph&& other) noexcept;
    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // Starts this frame's graph, between device.begin_frame() and end_frame(). reference_width and
    // reference_height size the transient textures with a reference_scale (the swapchain, or the
    // render resolution).
    void begin(uint32_t reference_width, uint32_t reference_height);
    // This frame's, as begin() set them.
    uint32_t reference_width() const;
    uint32_t reference_height() const;

    GraphTexture import_texture(const ImportedTexture& texture);
    GraphBuffer import_buffer(const ImportedBuffer& buffer);

    // setup(PassBuilder&), execute(PassContext&).
    template <typename Setup, typename Execute>
    void add_pass(const char* name, Setup&& setup, Execute&& execute);

    // setup(PassBuilder&, Data&) fills the pass's data; execute(const Data&, PassContext&) reads
    // it. The data stays valid (for later passes' setups too) until the next begin().
    template <typename Data, typename Setup, typename Execute>
    const Data& add_pass(const char* name, Setup&& setup, Execute&& execute);

    // Culls, records the passes and the barriers between them, and the imported resources' final
    // transitions.
    void execute();

    // The state an imported texture was left in by the last execute(): its final_state, or the
    // state its last uses left (General if those differ per subresource, with a warning).
    rhi::ResourceState final_texture_state(GraphTexture texture) const;

    RenderGraphStats stats() const;
    // Logs the last frame's graph: passes (culled ones too), their uses, the resources.
    void dump() const;

private:
    using ExecuteFn = void (*)(const void* closure, PassContext& context);

    explicit RenderGraph(RenderGraphState* state) : state_(state) {}
    uint32_t begin_pass(const char* name);
    PassBuilder builder(uint32_t pass) { return {state_, pass}; }
    void* allocate_closure(size_t size, size_t alignment);
    void end_pass(uint32_t pass, ExecuteFn execute_fn, const void* closure);

    RenderGraphState* state_ = nullptr;
};

template <typename Setup, typename Execute>
void RenderGraph::add_pass(const char* name, Setup&& setup, Execute&& execute) {
    using Closure = std::decay_t<Execute>;
    static_assert(std::is_trivially_destructible_v<Closure>,
                  "a pass's execute closure lives in the graph's frame arena and is never destroyed: capture "
                  "handles, pointers and values, not containers");
    static_assert(std::is_invocable_v<const Closure&, PassContext&>, "execute is called as execute(PassContext&)");
    const uint32_t pass = begin_pass(name);
    PassBuilder pass_builder = builder(pass);
    std::forward<Setup>(setup)(pass_builder);
    void* memory = allocate_closure(sizeof(Closure), alignof(Closure));
    const Closure* closure = ::new (memory) Closure(std::forward<Execute>(execute));
    end_pass(
        pass, [](const void* stored, PassContext& context) { (*static_cast<const Closure*>(stored))(context); },
        closure);
}

template <typename Data, typename Setup, typename Execute>
const Data& RenderGraph::add_pass(const char* name, Setup&& setup, Execute&& execute) {
    using Closure = std::decay_t<Execute>;
    static_assert(std::is_trivially_destructible_v<Data> && std::is_default_constructible_v<Data>,
                  "pass data lives in the graph's frame arena and is never destroyed: hold handles, pointers and "
                  "values, not containers");
    static_assert(std::is_trivially_destructible_v<Closure>,
                  "a pass's execute closure lives in the graph's frame arena and is never destroyed: capture "
                  "handles, pointers and values, not containers");
    static_assert(std::is_invocable_v<const Closure&, const Data&, PassContext&>,
                  "execute is called as execute(const Data&, PassContext&)");
    struct Stored {
        Closure execute;
        const Data* data;
    };
    const uint32_t pass = begin_pass(name);
    Data* data = ::new (allocate_closure(sizeof(Data), alignof(Data))) Data{};
    PassBuilder pass_builder = builder(pass);
    std::forward<Setup>(setup)(pass_builder, *data);
    void* memory = allocate_closure(sizeof(Stored), alignof(Stored));
    const Stored* stored = ::new (memory) Stored{std::forward<Execute>(execute), data};
    end_pass(
        pass,
        [](const void* closure, PassContext& context) {
            const auto* self = static_cast<const Stored*>(closure);
            self->execute(*self->data, context);
        },
        stored);
    return *data;
}

} // namespace de::render
