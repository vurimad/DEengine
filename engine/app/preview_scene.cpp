#include "app/preview_scene.h"

#include "core/log.h"
#include "core/profile.h"
#include "platform/time.h"
#include "render/rhi/device.h"
#include "world/render_extract.h"
#include "world/transform.h"

#include <cstring>
#include <new>
#include <utility>

namespace de::app {
namespace {

constexpr uint64_t kBytesPerTexel = 4;

core::Allocator& heap() {
    return core::heap_allocator(core::MemoryTag::Render);
}

// A picture on its way back: copied into a buffer by some frame, readable once the GPU has finished that frame.
struct Readback {
    rhi::BufferHandle buffer;
    uint64_t size = 0;
    bool used = false;    // copied to before: the graph is told how that left it
    bool waiting = false; // holds a picture nobody has taken
    uint64_t frame = 0;   // Device::frame_index() of the frame that copied it
    uint64_t tag = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// A picture that has come back and waits to be taken, out of the buffer the next one is copied into.
struct KeptPicture {
    core::Array<std::byte> texels{heap()};
    bool waiting = false;
    uint64_t frame = 0;
    uint64_t tag = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

} // namespace

struct PreviewSceneState {
    rhi::Device* device = nullptr;
    // In this order: the graph and the renderer go before the world whose frame they hold.
    world::World world;
    render::SceneRenderer renderer;
    render::RenderGraph graph;
    flecs::entity camera;
    rhi::TextureHandle target;
    rhi::ResourceState target_state = rhi::ResourceState::Undefined;
    uint32_t width = 0;
    uint32_t height = 0;
    Readback readbacks[rhi::kFramesInFlight];
    KeptPicture kept[rhi::kFramesInFlight]; // each buffer's last picture, moved aside when the buffer is used again
    bool extracted_undrawn = false;         // the last advance()'s frame never reached the renderer

    PreviewSceneState(rhi::Device& gpu, world::World&& its_world, render::SceneRenderer&& its_renderer,
                      render::RenderGraph&& its_graph)
        : device(&gpu), world(std::move(its_world)), renderer(std::move(its_renderer)), graph(std::move(its_graph)) {}
};

core::Result<PreviewScene> PreviewScene::create(const PreviewSceneDesc& desc) {
    if (desc.device == nullptr || desc.shaders == nullptr || desc.assets == nullptr || desc.gpu_assets == nullptr) {
        return core::fail(core::ErrorCode::InvalidArgument,
                          "a preview scene needs a device, a shader library, a registry and the GPU's assets");
    }
    const uint64_t start_ns = platform::time_ns();
    core::Result<world::World> world = world::World::create({.jobs = desc.jobs, .assets = desc.assets});
    if (!world) {
        return core::fail(world.error());
    }
    const uint64_t world_ns = platform::time_ns();
    // No uploader and no files: a preview has no ground and no water of its own.
    core::Result<render::SceneRenderer> renderer =
        render::SceneRenderer::create({.device = desc.device,
                                       .shaders = desc.shaders,
                                       .gpu_assets = desc.gpu_assets,
                                       .assets = desc.assets,
                                       .particle_pool_blocks = desc.particle_pool_blocks,
                                       .particle_catch_up_steps = desc.particle_catch_up_steps,
                                       .max_instances = desc.max_instances});
    if (!renderer) {
        return core::fail(renderer.error());
    }
    core::Result<render::RenderGraph> graph = render::RenderGraph::create(*desc.device);
    if (!graph) {
        return core::fail(graph.error());
    }
    auto* state = ::new (heap().allocate(sizeof(PreviewSceneState), alignof(PreviewSceneState)))
        PreviewSceneState(*desc.device, std::move(*world), std::move(*renderer), std::move(*graph));
    PreviewScene scene(state);
    state->camera = state->world.ecs().entity("preview_camera").set(world::Camera{});
    state->world.ecs().set(world::ActiveCamera{.entity = state->camera.id()});
    scene.set_camera({});
    const uint64_t end_ns = platform::time_ns();
    DE_LOG_DEBUG("render", "preview scene: its world made in %.1f ms, its renderer and graph in %.1f ms",
                 static_cast<double>(world_ns - start_ns) / static_cast<double>(platform::kNsPerMs),
                 static_cast<double>(end_ns - world_ns) / static_cast<double>(platform::kNsPerMs));
    return scene;
}

PreviewScene::~PreviewScene() {
    if (state_ == nullptr) {
        return;
    }
    state_->device->destroy(state_->target);
    for (const Readback& readback : state_->readbacks) {
        state_->device->destroy(readback.buffer);
    }
    state_->~PreviewSceneState();
    heap().deallocate(state_, sizeof(PreviewSceneState), alignof(PreviewSceneState));
}

PreviewScene::PreviewScene(PreviewScene&& other) noexcept : state_(other.state_) {
    other.state_ = nullptr;
}

PreviewScene& PreviewScene::operator=(PreviewScene&& other) noexcept {
    if (this != &other) {
        std::swap(state_, other.state_);
    }
    return *this;
}

world::World& PreviewScene::world() {
    return state_->world;
}

double PreviewScene::time_s() const {
    return state_->world.render_frame().time_s;
}

void PreviewScene::set_camera(const PreviewCamera& camera) {
    state_->camera.set(world::Camera{.vertical_fov_rad = camera.vertical_fov_rad, .near_m = camera.near_m});
    world::place(state_->camera, world::Transform{.position = camera.position, .rotation = camera.rotation});
}

void PreviewScene::advance(double seconds) {
    DE_PROFILE_ZONE("preview advance");
    if (state_->extracted_undrawn) {
        // The renderer never saw the last frame's changes: this one holds everything.
        state_->world.resync_render_instances();
    }
    static_cast<void>(state_->world.advance(seconds));
    state_->extracted_undrawn = true;
}

void PreviewScene::set_size(uint32_t width, uint32_t height) {
    PreviewSceneState& state = *state_;
    if (width == state.width && height == state.height) {
        return;
    }
    state.device->destroy(state.target); // once the GPU is done with it
    state.target = {};
    state.target_state = rhi::ResourceState::Undefined;
    state.width = 0;
    state.height = 0;
    if (width == 0 || height == 0) {
        return;
    }
    const core::Result<rhi::TextureHandle> made = state.device->create_texture(
        {.width = width,
         .height = height,
         .format = kFormat,
         .usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::Sampled | rhi::TextureUsage::CopySrc,
         .debug_name = "preview"});
    if (!made) {
        DE_LOG_ERROR("render", "no %ux%u preview: %s", width, height, made.error().message);
        return;
    }
    state.target = *made;
    state.width = width;
    state.height = height;
}

uint32_t PreviewScene::width() const {
    return state_->width;
}

uint32_t PreviewScene::height() const {
    return state_->height;
}

uint64_t PreviewScene::texture_id() const {
    return state_->target.is_valid() ? state_->device->texture_index(state_->target) : 0;
}

void PreviewScene::draw(const render::SceneLighting& lighting, bool taa, uint64_t read_back_tag) {
    DE_PROFILE_ZONE("preview draw");
    PreviewSceneState& state = *state_;
    if (!state.target.is_valid()) {
        return;
    }
    render::RenderGraph& graph = state.graph;
    graph.begin(state.width, state.height);
    const render::GraphTexture target = graph.import_texture({.texture = state.target,
                                                              .name = "preview",
                                                              .state = state.target_state,
                                                              .stages = rhi::ShaderStages::Fragment,
                                                              .final_state = rhi::ResourceState::ShaderRead,
                                                              .final_stages = rhi::ShaderStages::Fragment,
                                                              .discard = true});
    state.target_state = rhi::ResourceState::ShaderRead;
    state.renderer.add_passes(graph, target, kFormat, state.world.render_frame(), lighting, render::SceneView::Lit,
                              taa);
    state.extracted_undrawn = state.renderer.instances_stale();
    if (read_back_tag != 0) {
        // This frame slot's buffer. What it holds has been readable since this frame began (begin_frame waited for
        // the frame that copied it): it's moved aside, to be taken from there.
        const size_t slot = state.device->frame_index() % rhi::kFramesInFlight;
        Readback& readback = state.readbacks[slot];
        const uint64_t size = uint64_t{state.width} * state.height * kBytesPerTexel;
        if (readback.waiting) {
            KeptPicture& kept = state.kept[slot];
            if (kept.waiting) {
                DE_LOG_WARN("render", "a preview's picture was never taken: a later one takes its place");
            }
            state.device->invalidate_mapped(readback.buffer);
            kept.texels.resize(static_cast<size_t>(readback.size));
            std::memcpy(kept.texels.data(), state.device->mapped(readback.buffer).data(),
                        static_cast<size_t>(readback.size));
            kept.waiting = true;
            kept.frame = readback.frame;
            kept.tag = readback.tag;
            kept.width = readback.width;
            kept.height = readback.height;
            readback.waiting = false;
        }
        if (readback.size != size) {
            state.device->destroy(readback.buffer);
            readback = Readback{};
            const core::Result<rhi::BufferHandle> made =
                state.device->create_buffer({.size = size,
                                             .usage = rhi::BufferUsage::CopyDst,
                                             .memory = rhi::MemoryLocation::Readback,
                                             .debug_name = "preview readback"});
            if (made) {
                readback.buffer = *made;
                readback.size = size;
            } else {
                DE_LOG_ERROR("render", "no preview readback: %s", made.error().message);
            }
        }
        if (readback.buffer.is_valid()) {
            const render::GraphBuffer buffer = graph.import_buffer(
                {.buffer = readback.buffer,
                 .name = "preview readback",
                 .state = readback.used ? rhi::ResourceState::HostRead : rhi::ResourceState::Undefined,
                 .final_state = rhi::ResourceState::HostRead});
            readback.used = true;
            readback.waiting = true;
            readback.frame = state.device->frame_index();
            readback.tag = read_back_tag;
            readback.width = state.width;
            readback.height = state.height;
            graph.add_pass(
                "preview readback",
                [&](render::PassBuilder& pass) {
                    pass.use(target, rhi::ResourceState::CopySrc);
                    pass.use(buffer, rhi::ResourceState::CopyDst);
                },
                [target, buffer](render::PassContext& context) {
                    context.list.copy_texture_to_buffer(context.texture(target), context.buffer(buffer));
                });
        }
    }
    graph.execute();
}

bool PreviewScene::take_picture(core::Array<std::byte>& rgba, uint32_t& width, uint32_t& height, uint64_t& tag) {
    PreviewSceneState& state = *state_;
    // The oldest there is. One moved aside is older than any still in a buffer.
    KeptPicture* kept = nullptr;
    for (KeptPicture& picture : state.kept) {
        if (picture.waiting && (kept == nullptr || picture.frame < kept->frame)) {
            kept = &picture;
        }
    }
    if (kept != nullptr) {
        rgba.resize(kept->texels.size());
        std::memcpy(rgba.data(), kept->texels.data(), kept->texels.size());
        width = kept->width;
        height = kept->height;
        tag = kept->tag;
        kept->waiting = false;
        return true;
    }
    // Else one whose frame the GPU has finished: begin_frame() waits for the frame kFramesInFlight back.
    Readback* oldest = nullptr;
    for (Readback& readback : state.readbacks) {
        if (readback.waiting && state.device->frame_index() >= readback.frame + rhi::kFramesInFlight &&
            (oldest == nullptr || readback.frame < oldest->frame)) {
            oldest = &readback;
        }
    }
    if (oldest == nullptr) {
        return false;
    }
    state.device->invalidate_mapped(oldest->buffer);
    const std::span<const std::byte> texels = state.device->mapped(oldest->buffer);
    rgba.resize(static_cast<size_t>(oldest->size));
    std::memcpy(rgba.data(), texels.data(), static_cast<size_t>(oldest->size));
    width = oldest->width;
    height = oldest->height;
    tag = oldest->tag;
    oldest->waiting = false;
    return true;
}

uint32_t PreviewScene::pictures_pending() const {
    uint32_t pending = 0;
    for (const Readback& readback : state_->readbacks) {
        pending += readback.waiting ? 1U : 0U;
    }
    for (const KeptPicture& picture : state_->kept) {
        pending += picture.waiting ? 1U : 0U;
    }
    return pending;
}

render::SceneRendererStats PreviewScene::stats() const {
    return state_->renderer.stats();
}

std::span<const render::PassGpuTime> PreviewScene::gpu_times() const {
    return state_->graph.gpu_times();
}

render::SceneRenderer& PreviewScene::renderer() {
    return state_->renderer;
}

} // namespace de::app
