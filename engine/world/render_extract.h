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
    float transform[12] = {}; // 3x4, rows x, y, z, translation in the last column
    asset::AssetHandle mesh;
    uint32_t reserved[2] = {};
};
static_assert(sizeof(RenderInstance) == 64);

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
    float alpha = 0.0f;        // between the last tick and the next
    DVec3 origin{0.0};         // the render origin: what instance positions are relative to
    uint64_t origin_moves = 0; // counts origin moves: GPU copies made relative to it go stale
    RenderView camera;
    core::Array<RenderInstance> instances{core::heap_allocator(core::MemoryTag::Render)};
};

// The render origin follows the camera in steps of this, so floats near it stay precise (0.06 mm at
// 1 km) and it moves rarely.
inline constexpr double kRenderOriginStep = 1024.0;

void register_render_components(flecs::world& ecs);

// Adds the RenderExtract system that fills frame (which must outlive the world): interpolated
// world transforms of everything with a MeshInstance, and the active camera, relative to the
// render origin. Rows are split across jobs (null: one thread). After the first frames, it doesn't
// allocate (the instance array keeps its capacity).
void add_render_extract(flecs::world& ecs, flecs::entity phase, RenderFrame& frame, core::JobSystem* jobs);

} // namespace de::world
