#pragma once

#include "asset/registry.h"
#include "core/array.h"
#include "core/math.h"
#include "world/transform.h"

#include <flecs.h>

#include <cstdint>

namespace de::core {
class JobSystem;
}

namespace de::world {

// Draws a mesh where the entity is. The handle doesn't hold the mesh: whatever spawned the entity
// does (its cell), and outlives it.
struct MeshInstance {
    asset::AssetHandle mesh;
};

// A point of view. The active one (ActiveCamera) is what render extraction looks through.
struct Camera {
    float vertical_fov_rad = 1.0f;
    float near_m = 0.1f; // reverse Z, no far plane
};

// Singleton: the camera entity the frame is seen through.
struct ActiveCamera {
    flecs::entity_t entity = 0;
};

// One mesh to draw this frame: where (relative to the frame's render origin) and what.
struct RenderInstance {
    float transform[12] = {};          // 3x4, rows x, y, z, translation in the last column
    float previous_transform[12] = {}; // where it was a frame ago (for motion vectors), same origin
    asset::AssetHandle mesh;
};
static_assert(sizeof(RenderInstance) == 104);

struct RenderView {
    float transform[12] = {}; // the camera's 3x4, relative to the render origin
    float vertical_fov_rad = 1.0f;
    float near_m = 0.1f;
    bool valid = false; // no active camera: nothing to look through
};

// What RenderExtract hands the renderer each frame (docs/design.md, World model): the world seen
// at this frame's interpolation point, as floats relative to a render origin. The renderer owns no
// list of entities; it reads this, rebuilt every frame.
struct RenderFrame {
    DVec3 origin{0.0};         // the render origin: what instance positions are relative to
    uint64_t origin_moves = 0; // counts origin moves: GPU copies made relative to it go stale
    RenderView camera;
    core::Array<RenderInstance> instances{core::heap_allocator(core::MemoryTag::Render)};
};

// The render origin follows the camera in steps of this, so floats near it stay precise (0.06 mm at
// 1 km) and it moves rarely.
inline constexpr double kRenderOriginStep = 1024.0;

void register_render_components(flecs::world& ecs);

// Extraction's working memory, kept between frames so it doesn't allocate once settled.
struct RenderExtractScratch {
    struct Chunk {
        const WorldTransform* current = nullptr;
        const PreviousWorldTransform* previous = nullptr;
        const MeshInstance* meshes = nullptr;
        uint32_t count = 0;
        uint32_t first = 0; // where its rows go in RenderFrame::instances
    };
    core::Array<Chunk> chunks{core::heap_allocator(core::MemoryTag::Render)};
};

struct RenderExtractDesc {
    RenderFrame* frame = nullptr;            // filled; must outlive the world
    RenderExtractScratch* scratch = nullptr; // must outlive the world
    const float* alpha = nullptr;            // this frame's interpolation point, set before RenderExtract
    const float* alpha_step = nullptr;       // how much of a tick this frame took, set likewise
    core::JobSystem* jobs = nullptr;         // null: one thread
};

// Adds the RenderExtract systems that fill desc.frame: the active camera, then interpolated world
// transforms of everything with a MeshInstance, relative to the render origin. Each instance's pose
// a frame ago comes from the same two ticks, stepped back by alpha_step: no history per entity, exact
// for steady motion, and none for what hasn't moved or was place()d. Every table's rows are cut into
// chunks and run as one parallel job over all of them. After the first frames it doesn't allocate
// (the arrays keep their capacity).
void add_render_extract(flecs::world& ecs, flecs::entity phase, const RenderExtractDesc& desc);

} // namespace de::world
