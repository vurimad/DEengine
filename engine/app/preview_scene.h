#pragma once

#include "asset/registry.h"
#include "core/array.h"
#include "core/math.h"
#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"
#include "render/scene/scene_renderer.h"
#include "world/world.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::core {
class JobSystem;
}
namespace de::rhi {
class Device;
}
namespace de::shader {
class ShaderLibrary;
}
namespace de::render {
class GpuAssets;
}

namespace de::app {

// A small scene of its own, drawn into a texture (docs/design.md, Preview scenes): a world apart from the game's,
// with the engine's own renderer looking at it, so what it shows is what the game would show. For a thing seen by
// itself: the editor's pictures of effects and prefabs, its effect viewer; later a character in a menu.
//
// It shares what's loaded with the game (the registry and the GPU's copies of meshes, textures and materials), and
// nothing else: its world has its own entities and time, its renderer its own instances, particles and history, its
// graph its own size. What's put in its world is put there as anywhere (cells aside): entities with a MeshInstance,
// an EffectInstance, a PrefabInstance.
//
// Each frame it's wanted: advance() once, then draw() between Device::begin_frame() and the frame's own graph
// (Application::draw), so the frame's UI can show the texture it drew that same frame.
struct PreviewSceneDesc {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    asset::AssetRegistry* assets = nullptr;
    render::GpuAssets* gpu_assets = nullptr;
    core::JobSystem* jobs = nullptr;
    uint32_t max_instances = 4096;                               // of meshes: 100 bytes each
    uint32_t particle_pool_blocks = render::kParticlePoolBlocks; // 2 KiB each
    // How many steps a frame an emitter behind its clock catches up by: more where an effect's time is dragged about.
    uint32_t particle_catch_up_steps = render::kParticleCatchUpSteps;
};

struct PreviewCamera {
    DVec3 position{0.0};
    Quat rotation{1.0f, 0.0f, 0.0f, 0.0f}; // looks down its -Z
    float vertical_fov_rad = 0.6f;
    float near_m = 0.02f;
};

struct PreviewSceneState;

// On the thread that renders. What it was made with must outlive it. Move-only.
class PreviewScene {
public:
    // The texture's format: 8 bits a channel, sRGB, as a picture is kept.
    static constexpr rhi::Format kFormat = rhi::Format::RGBA8Srgb;

    static core::Result<PreviewScene> create(const PreviewSceneDesc& desc);

    ~PreviewScene();
    PreviewScene(PreviewScene&& other) noexcept;
    PreviewScene& operator=(PreviewScene&& other) noexcept;
    PreviewScene(const PreviewScene&) = delete;
    PreviewScene& operator=(const PreviewScene&) = delete;

    world::World& world();
    // The world's time as of the last advance(): what an EffectClock is set against.
    double time_s() const;

    // Where it's seen from, from the next advance() on: put there at once, not moved there.
    void set_camera(const PreviewCamera& camera);

    // Its world's time moves on by this much (0: only what was put in it is taken in), and what it shows is made
    // ready for draw().
    void advance(double seconds);

    // The texture, at this size from now on (made again if it differs: outside a frame's GPU work, so before the UI
    // that shows it is built). 0 by 0 lets it go.
    void set_size(uint32_t width, uint32_t height);
    uint32_t width() const;
    uint32_t height() const;
    // For ImGui::Image; 0 while there's no texture.
    uint64_t texture_id() const;

    // Draws its world into the texture. read_back_tag, if not 0, asks for the picture's texels back: they come some
    // frames later, with the tag, from take_picture(), which whoever asks calls every frame (a picture left untaken
    // for several frames gives way to a later one). Nothing is drawn without a size.
    void draw(const render::SceneLighting& lighting, bool taa = false, uint64_t read_back_tag = 0);

    // The oldest picture that has come back, if any: its texels (RGBA, sRGB, rows from the top) into rgba.
    bool take_picture(core::Array<std::byte>& rgba, uint32_t& width, uint32_t& height, uint64_t& tag);
    // Pictures asked for and not yet taken.
    uint32_t pictures_pending() const;

    // What the last draw() did.
    render::SceneRendererStats stats() const;
    // GPU time of each of its passes, in a recent frame (RenderGraph::gpu_times).
    std::span<const render::PassGpuTime> gpu_times() const;
    // The renderer, for what a preview sets on it (its debug views).
    render::SceneRenderer& renderer();

private:
    explicit PreviewScene(PreviewSceneState* state) : state_(state) {}

    PreviewSceneState* state_ = nullptr;
};

} // namespace de::app
