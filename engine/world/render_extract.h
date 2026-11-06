#pragma once

#include "asset/registry.h"
#include "core/array.h"
#include "core/hash_map.h"
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

// An entity's place in the renderer's instance data, which stays between frames. Comes and goes with its MeshInstance,
// given and freed by observers.
struct RenderSlot {
    uint32_t slot = 0;
    uint64_t sent = 0; // a hash of what the renderer was last sent: what a write to its table may not have changed
};

// Fog in the shape of a box or an ellipsoid around the entity: its world scale is its half extents.
struct FogVolume {
    float density_per_m = 0.1f;         // extinction at its heart, on top of the world's fog
    float edge_m = 1.0f;                // how far in from its surface it takes to reach that
    float tint[3] = {1.0f, 1.0f, 1.0f}; // of the light it scatters
    bool ellipsoid = false;
    float noise_size_m = 8.0f;               // how big its noise's largest shapes are
    float noise_amount = 0.0f;               // 0: smooth, to 1: as much as noise can eat away
    float drift_mps[3] = {0.0f, 0.0f, 0.0f}; // how its noise moves through it: wind, in the world's axes
    float height_fade_m = 0.0f;              // how far down from its top it thins from nothing; 0: not at all
};

// A point of view. The active one (ActiveCamera) is what render extraction looks through.
struct Camera {
    float vertical_fov_rad = 1.0f;
    float near_m = 0.1f; // reverse Z, no far plane
};

// Singleton: the camera entity the frame is seen through. Set cut when the view jumps (a teleport, a
// cinematic cut): the next frame has no camera motion, instead of a streak across the screen. Changing
// entity does the same by itself. Extraction clears it.
struct ActiveCamera {
    flecs::entity_t entity = 0;
    bool cut = false;
};

// Where an instance goes in the renderer's data, which stays between frames.
inline constexpr uint32_t kAutoRenderSlot = UINT32_MAX;         // its index in RenderFrame::instances
inline constexpr uint32_t kSkipRenderInstance = UINT32_MAX - 1; // nothing to do for this row

// Positions the GPU keeps are the world's, in whole steps of 1/16384 m: 0.06 mm, what a float has at 1 km. They hold no
// render origin, so it can move with nothing sent. They wrap at 32 bits, and what's read back is a wrapping subtraction
// from the origin, so the world has no limit: only what's drawn has to be within 131 km of the origin.
inline constexpr double kRenderStepsPerM = 16384.0;
inline constexpr double kRenderReachM = 2147483648.0 / kRenderStepsPerM;

inline uint32_t render_position_steps(double position_m) {
    const double steps = position_m * kRenderStepsPerM;
    return static_cast<uint32_t>(static_cast<int64_t>(steps + (steps < 0.0 ? -0.5 : 0.5)));
}

// Where an instance is, as the GPU keeps it (InstanceTransform in shaders/scene/scene_common.slang).
struct RenderTransform {
    struct Row {
        float linear[3] = {};        // rotation and scale
        uint32_t position_steps = 0; // render_position_steps
    };
    Row rows[3] = {};

    void set_position_m(int axis, double position_m) { rows[axis].position_steps = render_position_steps(position_m); }

    // Relative to an origin, as the GPU works it out.
    float position_m(int axis, const DVec3& origin = DVec3(0.0)) const {
        const auto steps = static_cast<int32_t>(rows[axis].position_steps - render_position_steps(origin[axis]));
        return static_cast<float>(static_cast<double>(steps) / kRenderStepsPerM);
    }
};
static_assert(sizeof(RenderTransform) == 48);

// A mesh to draw from now on: where and what.
struct RenderInstance {
    RenderTransform transform = {};
    RenderTransform previous_transform = {}; // where it was a frame ago (for motion vectors)
    asset::AssetHandle mesh;
    uint32_t slot = kAutoRenderSlot;
};
static_assert(sizeof(RenderInstance) == 108);

struct RenderView {
    float transform[12] = {}; // the camera's 3x4, relative to the render origin
    float vertical_fov_rad = 1.0f;
    float near_m = 0.1f;
    bool valid = false; // no active camera: nothing to look through
    // The view a frame ago, for motion vectors, relative to this frame's origin: what was actually
    // drawn (a free camera is place()d every frame, so its ticks don't say). Not valid after a cut or
    // on the first frame: no camera motion then.
    float previous_transform[12] = {};
    float previous_vertical_fov_rad = 1.0f;
    float previous_near_m = 0.1f;
    bool previous_valid = false;
};

// One fog volume this frame, relative to the frame's render origin.
struct RenderFogVolume {
    float to_local[12] = {}; // rows: a point relative to the origin into the volume's own axes, in meters
    float center[3] = {};    // relative to the origin
    float half_extents[3] = {1.0f, 1.0f, 1.0f};
    float density_per_m = 0.1f;
    float edge_m = 1.0f;
    float tint[3] = {1.0f, 1.0f, 1.0f};
    bool ellipsoid = false;
    float noise_tiles_per_m = 0.0f;
    float noise_amount = 0.0f;
    float noise_offset_tiles[3] = {}; // past the render origin, in the world's axes, wrapped to [0, 4)
    float height_fade_m = 0.0f;
};

// What RenderExtract hands the renderer each frame (docs/design.md, World model): the world seen
// at this frame's interpolation point: the camera and fog as floats relative to a render origin, instances as
// RenderTransforms, which the GPU makes relative to it. The renderer owns no list of entities. Instances are what
// changed since the last frame, for it to keep: all of them only when the renderer asks
// (World::resync_render_instances).
struct RenderFrame {
    uint64_t number = 0; // counts extractions: a frame not drawn (minimized) leaves a gap
    DVec3 origin{0.0};   // the render origin: what the GPU's floats are relative to
    RenderView camera;
    flecs::entity_t camera_entity = 0; // whose view it is: another's next frame is a cut
    core::Array<RenderInstance> instances{core::heap_allocator(core::MemoryTag::Render)};
    core::Array<uint32_t> freed_slots{core::heap_allocator(core::MemoryTag::Render)}; // draw nothing from now on
    uint32_t slot_count = 0;    // slots in use may be below this. 0: instances.size()
    bool all_instances = false; // these are all there are: forget any others
    bool instance_out_of_reach =
        false; // one of these is past kRenderReachM from the origin, so it draws in the wrong place
    core::Array<RenderFogVolume> fog_volumes{core::heap_allocator(core::MemoryTag::Render)};
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
        RenderSlot* slots = nullptr;
        uint64_t table = 0;
        uint32_t count = 0;
        uint32_t first = 0;     // where its rows go in RenderFrame::instances
        bool write_all = false; // something wrote to its table: every row is sent, not only what's moving
        bool any_moving = false;
        bool out_of_reach = false;
    };
    core::Array<Chunk> chunks{core::heap_allocator(core::MemoryTag::Render)};
    // The frame each table last had something moving in it: what to look at though nothing wrote to it.
    core::HashMap<uint64_t, uint64_t> moving_tables{core::heap_allocator(core::MemoryTag::Render)};
    core::Array<uint32_t> free_slots{core::heap_allocator(core::MemoryTag::Render)};
    core::Array<uint32_t> freed_slots{core::heap_allocator(core::MemoryTag::Render)}; // since the last frame
    uint32_t next_slot = 0;
    bool resync = false; // send all of them, next frame
    bool warned_out_of_reach = false;
};

struct RenderExtractDesc {
    RenderFrame* frame = nullptr;            // filled; must outlive the world
    RenderExtractScratch* scratch = nullptr; // must outlive the world
    const float* alpha = nullptr;            // this frame's interpolation point, set before RenderExtract
    const float* alpha_step = nullptr;       // how much of a tick this frame took, set likewise
    const double* time_s = nullptr;          // the world's time at this frame's interpolation point, likewise
    core::JobSystem* jobs = nullptr;         // null: one thread
};

// Adds the RenderExtract systems that fill desc.frame: the active camera, then interpolated world
// transforms of what changed with a MeshInstance. Each instance's pose
// a frame ago comes from the same two ticks, stepped back by alpha_step (a frame longer than a tick
// goes back past the previous tick, by the same motion): no history per entity, exact for steady
// motion, and none for what hasn't moved or was place()d.
//
// A table nothing wrote to, with nothing moving in it, costs next to nothing, whatever its size. In a table
// something wrote to, which spawning anything into it does, each row is checked against a hash of what it last
// sent. What stopped moving is sent once more when its previous transform catches up, in a tick: that row has no
// motion in it. Those tables' rows are cut into chunks and run as one parallel job. After the first frames it doesn't
// allocate (the arrays keep their capacity).
//
// What's sent past kRenderReachM from the origin is reported, and logged once. What the camera left that far behind
// since it was sent isn't: streaming has to have unloaded it long before.
//
// An entity leaves the renderer when it loses its MeshInstance or goes. Disabling it (flecs::Disabled) only stops
// updates: it stays drawn where it was.
void add_render_extract(flecs::world& ecs, flecs::entity phase, const RenderExtractDesc& desc);

} // namespace de::world
