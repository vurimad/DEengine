#pragma once

#include "asset/registry.h"
#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::rhi {
class Device;
}
namespace de::shader {
class ShaderLibrary;
}

namespace de::render {

class GpuAssets;
struct GpuMesh;

// What rays are traced against (docs/design.md, Global illumination): a bottom-level acceleration structure for
// each mesh some instance uses, built once from the geometry pool, and one top level over the frame's instances,
// built again every frame from where they are. Only where the GPU has ray queries (Capabilities::ray_query).
//
// The CPU works per mesh (which need a bottom level, which lost theirs to an unload or a reload); the GPU per
// instance: a compute pass writes the top level's instances from the renderer's own instance table
// (shaders/scene/trace_instances.slang), so nothing here is per instance on the CPU. Shaders trace through
// shaders/scene/trace.slang.

// A mesh as rays see it, by its slot in the mesh table (MeshTrace in shaders/scene/scene_common.slang).
struct GpuMeshTrace {
    uint64_t bottom_level = 0; // its acceleration structure's address; 0: none yet, and no ray meets its instances
    uint32_t first_part = 0;   // in the part table: the parts of the level of detail rays see, a run each
    uint32_t part_count = 0;
};
static_assert(sizeof(GpuMeshTrace) == 16);

// What a shader that traces reads, a frame's (TraceScene in shaders/scene/scene_common.slang).
struct GpuTraceScene {
    uint64_t top_level = 0; // 0: nothing built yet
    uint64_t instances = 0; // the renderer's instance table, by slot
    uint64_t instance_meshes = 0;
    uint64_t mesh_trace = 0; // GpuMeshTrace by mesh slot
    uint64_t parts = 0;      // the part table
    uint64_t vertices = 0;   // the geometry pool
    uint64_t indices = 0;
    uint64_t materials = 0;
};
static_assert(sizeof(GpuTraceScene) == 64);

// Rays see the coarsest level of detail of a mesh that lies within this of its finest, has all of its parts and kept
// its seams (asset::MeshLod::seams_moved): less to build and to walk, and nothing a bounce of light or a reflection
// can tell. A mesh with its uvs in pieces (a scan, a generated model) has no such level, and is traced as it's drawn
// up close.
inline constexpr float kTraceLodErrorM = 0.02f;
// Which level of detail of a mesh that is.
size_t trace_lod(const GpuMesh& mesh);
// The most instance slots the top level takes: those past it aren't traced. It holds every instance below that,
// near or far, which suits a hall and not a world: a world's wants only what's near the camera, later.
inline constexpr uint32_t kMaxTraceInstances = 65536;
// Bottom levels built in one frame at most: a world coming in spreads over frames.
inline constexpr uint32_t kTraceBuildsPerFrame = 16;

struct TraceSceneDesc {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
    GpuAssets* gpu_assets = nullptr;
    asset::AssetRegistry* assets = nullptr; // optional: with it, a mesh cooked again gets a new bottom level
    uint32_t max_instances = kMaxTraceInstances;
    uint32_t max_meshes = 16384; // GpuAssetsDesc::max_meshes
};

// What its passes need of the renderer they're added to.
struct TracePassInput {
    uint64_t frame_address = 0; // the scene's SceneFrame, written
    GraphBuffer instance_data;  // the renderer's instance table and their meshes, as its update pass left them
    GraphBuffer instance_meshes;
    uint64_t instance_data_address = 0;
    uint64_t instance_meshes_address = 0;
    uint32_t slots = 0; // instance slots in use
};

struct TraceStats {
    uint32_t meshes = 0;    // with a bottom level
    uint32_t waiting = 0;   // wanted, not built yet (not on the GPU yet, or over the frame's share)
    uint32_t builds = 0;    // bottom levels built this frame
    uint32_t instances = 0; // slots in the top level this frame
    uint64_t triangles = 0; // in the bottom levels there are
};

struct TraceSceneState;

// On the thread that renders. What it was made with must outlive it. Move-only.
class TraceScene {
public:
    // Fails (Unsupported) on a GPU without ray queries: the renderer goes on without tracing.
    static core::Result<TraceScene> create(const TraceSceneDesc& desc);

    ~TraceScene();
    TraceScene(TraceScene&& other) noexcept;
    TraceScene& operator=(TraceScene&& other) noexcept;
    TraceScene(const TraceScene&) = delete;
    TraceScene& operator=(const TraceScene&) = delete;

    // An instance uses this mesh (its slot in the mesh table): it gets a bottom level once it's on the GPU.
    void want(uint32_t mesh_slot, asset::AssetHandle mesh);
    // Where this frame's GpuTraceScene is, for SceneFrame::trace: written by update().
    uint64_t scene_address() const;
    // Each frame, once the frame's instances have said what they want: bottom levels for the meshes that are ready
    // (a frame's share), those of meshes no instance uses now, that went or were cooked again let go, and this
    // frame's GpuTraceScene written. mesh_instances: how many instances use each mesh slot.
    void update(uint64_t instance_data_address, uint64_t instance_meshes_address,
                std::span<const uint32_t> mesh_instances);
    // Adds "trace bottom levels" (what update() queued), "trace instances" and "trace top level": after the
    // renderer's instance update, before anything traces. The top level is in step with this frame's instances.
    void add_passes(RenderGraph& graph, const TracePassInput& input);

    TraceStats stats() const;

private:
    explicit TraceScene(TraceSceneState* state) : state_(state) {}

    TraceSceneState* state_ = nullptr;
};

} // namespace de::render
