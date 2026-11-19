#pragma once

#include "asset/light_format.h"
#include "asset/registry.h"
#include "core/array.h"
#include "core/hash_map.h"
#include "core/math.h"
#include "world/transform.h"

#include <flecs.h>

#include <algorithm>
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
    uint64_t moved_frame = 0; // the frame it was last sent from MovedRows, which a look through its table leaves alone
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
    float under_roof = -1.0f; // how far its place is closed to the sky, 0 to 1 (asset::CellFogVolume); negative: unsaid
};

// A lake or a pond: still water, its surface flat at the entity's height, as wide as its world scale along x and z
// (half extents), turned with its yaw. It shows only where the ground is lower than it.
struct WaterBody {
    bool ellipse = true; // else a rectangle
    float waves = 0.15f; // 0: glassy, to 1: the sea's own waves
};

// An effect's own clock (docs/design.md, VFX foundation): its age follows the world's time from a moment it was set
// at, at its own rate. Its particles are what that age gives, whenever and however it got there: held still, slowed,
// set back or far ahead, it shows exactly what playing to that age would have left. The default is as old as the
// world: what stands in a cell has always been burning, whenever its cell came in. What starts at some moment (a
// burst, an impact) is set to that moment with start().
struct EffectClock {
    double since_s = 0.0; // the world's time it counts from
    double age_s = 0.0;   // how old it was then
    float rate = 1.0f;    // of the world's time: 0 holds it still, 0.5 is half speed

    // How old it is at a world's time: never younger than nothing.
    double age_at(double time_s) const {
        return std::max(0.0, age_s + ((time_s - since_s) * static_cast<double>(rate)));
    }
    // From a world's time on: at this age, and then as before.
    void seek(double time_s, double age) {
        since_s = time_s;
        age_s = std::max(0.0, age);
    }
    // From a world's time on: at another rate, from the age it has reached.
    void set_rate(double time_s, float new_rate) {
        seek(time_s, age_at(time_s));
        rate = std::max(0.0f, new_rate);
    }
    // Starts it at a world's time: new, at its usual rate.
    void start(double time_s) { *this = EffectClock{.since_s = time_s}; }
};

// Plays an effect where the entity is (docs/design.md, VFX foundation): its emitters spawn there, in the entity's
// axes, at its scale. The handle doesn't hold the effect: whatever spawned the entity does (its cell).
struct EffectInstance {
    asset::AssetHandle effect; // an asset::EffectAsset
    EffectClock clock{};
    // Emitters left out of the picture, a bit each by their place in the effect: they go on being simulated. For
    // looking at one of them by itself (the editor's effect viewer).
    uint32_t hidden_emitters = 0;
};

// A local light where the entity is (docs/design.md, Local lights): a point that shines every way, or with a cone a
// spot along the entity's -Z. As asset::LightDesc, which is what files keep. It scales with its entity as if the whole
// thing were made bigger: its radius by the scale, its light by the scale squared, so a prefab placed at half size is
// lit the same, at half size.
struct LocalLight {
    float color[3] = {1.0f, 1.0f, 1.0f}; // linear
    float intensity = 1.0f;  // what it gives a surface facing it from 1 m, in the sun's units; falls with the square
    float radius_m = 4.0f;   // where its light has come down to nothing
    float cone_deg = 0.0f;   // 0: a point light; else a spot's angle from its axis to where it ends
    float cone_soft = 0.25f; // how much of that angle is the fade at its edge
    float flicker = 0.0f;    // 0: steady, to 1: the share of its light a flicker takes away at most
    float flicker_hz = 8.0f;
    float shadow = 1.0f; // how much of a shadow it casts: 0 none (asset::LightDesc)

    bool operator==(const LocalLight&) const = default;

    static LocalLight from(const asset::LightDesc& light) {
        return {.color = {light.color[0], light.color[1], light.color[2]},
                .intensity = light.intensity,
                .radius_m = light.radius_m,
                .cone_deg = light.cone_deg,
                .cone_soft = light.cone_soft,
                .flicker = light.flicker,
                .flicker_hz = light.flicker_hz,
                .shadow = light.shadow};
    }
    asset::LightDesc desc() const {
        return {.color = {color[0], color[1], color[2]},
                .intensity = intensity,
                .radius_m = radius_m,
                .cone_deg = cone_deg,
                .cone_soft = cone_soft,
                .flicker = flicker,
                .flicker_hz = flicker_hz,
                .shadow = shadow};
    }
};
static_assert(sizeof(LocalLight) == sizeof(asset::LightDesc));

// How far along a flicker a light is at a time, from 0 to 1: three slow waves that never line up, each light (seed)
// at its own place in them. The same time gives the same value.
float light_flicker(double time_s, float hz, uint32_t seed);

// An entity's place among effects, which the renderer keeps its particles by between frames. Comes and goes with its
// EffectInstance, given and freed by observers.
struct EffectSlot {
    uint32_t slot = 0;
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
    float under_roof = -1.0f; // negative: it doesn't say
};

// One lake or pond this frame, relative to the frame's render origin.
struct RenderWaterBody {
    float center[3] = {};                 // x, z: its middle; y: its surface
    float half_extents[2] = {1.0f, 1.0f}; // along its own x and z, in meters
    float axis[2] = {1.0f, 0.0f};         // its own x in the world's (x, z): its yaw
    bool ellipse = true;
    float waves = 0.15f;
};

// One effect this frame (docs/design.md, VFX foundation): which, where, and whose. An effect that isn't in a frame's
// list is gone: its particles go with it.
struct RenderEffect {
    RenderTransform transform = {}; // where its emitters are: its entity's place
    asset::AssetHandle effect;      // an asset::EffectAsset
    uint32_t slot = 0; // its entity's place among effects (EffectSlot): the same every frame, so its particles carry on
    uint32_t seed = 0; // what makes its particles differ from another's of the same effect
    EffectClock clock{};          // how old it is at the frame's time (RenderFrame::time_s)
    uint32_t hidden_emitters = 0; // EffectInstance's
};

// One local light this frame, relative to the frame's render origin, as it shines now (its flicker taken in).
struct RenderLight {
    float position[3] = {};
    float radius_m = 1.0f;
    float color[3] = {};             // times its intensity
    float direction[3] = {0, 0, -1}; // a spot's axis: the way it shines
    float cos_outer = -2.0f;         // of a spot's angle; a point light: below any cosine
    float cone_scale = 1.0f;         // 1 / (the cosine of where its edge starts to fade - cos_outer); a point light: 1
    float shadow = 1.0f;             // how much of a shadow it casts, 0 to 1: at 0 it takes no shadow tiles
};

// What RenderExtract hands the renderer each frame (docs/design.md, World model): the world seen
// at this frame's interpolation point: the camera and fog as floats relative to a render origin, instances as
// RenderTransforms, which the GPU makes relative to it. The renderer owns no list of entities. Instances are what
// changed since the last frame, for it to keep: all of them only when the renderer asks
// (World::resync_render_instances).
struct RenderFrame {
    uint64_t number = 0;          // counts extractions: a frame not drawn (minimized) leaves a gap
    DVec3 origin{0.0};            // the render origin: what the GPU's floats are relative to
    double time_s = 0.0;          // the world's time at this frame's interpolation point: what wind moves by
    double previous_time_s = 0.0; // and at the frame drawn before it; after a cut, or at first, this frame's
    RenderView camera;
    flecs::entity_t camera_entity = 0; // whose view it is: another's next frame is a cut
    core::Array<RenderInstance> instances{core::heap_allocator(core::MemoryTag::Render)};
    core::Array<uint32_t> freed_slots{core::heap_allocator(core::MemoryTag::Render)}; // draw nothing from now on
    uint32_t slot_count = 0;    // slots in use may be below this. 0: instances.size()
    bool all_instances = false; // these are all there are: forget any others
    bool instance_out_of_reach =
        false; // one of these is past kRenderReachM from the origin, so it draws in the wrong place
    core::Array<RenderFogVolume> fog_volumes{core::heap_allocator(core::MemoryTag::Render)};
    core::Array<RenderWaterBody> water_bodies{core::heap_allocator(core::MemoryTag::Render)};
    core::Array<RenderEffect> effects{core::heap_allocator(core::MemoryTag::Render)}; // every one there is
    core::Array<RenderLight> lights{core::heap_allocator(core::MemoryTag::Render)};   // every one there is
};

// The render origin follows the camera in steps of this, so floats near it stay precise (0.06 mm at
// 1 km) and it moves rarely.
inline constexpr double kRenderOriginStep = 1024.0;

void register_render_components(flecs::world& ecs);

// Rows moved one at a time (move_root), which mark no table: how extraction hears of them, so that a few moving in a
// big table cost what the few do. Report what moved every tick, even if that's nothing. They're sent each frame until a
// tick after that, as what's drawn moves between ticks. One writer, on the game thread.
struct MovedRows {
    core::Array<flecs::entity_t> entities{core::heap_allocator(core::MemoryTag::Render)};
    uint32_t reports = 0; // since extraction last looked: more than one can name the same row twice
    bool seen = false;    // by a frame, so the next report starts over

    void report(std::span<const flecs::entity_t> moved);
};

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
    core::Array<uint32_t> free_effect_slots{core::heap_allocator(core::MemoryTag::Render)};
    uint32_t next_effect_slot = 0;
    MovedRows moved_rows;
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
// What's reported to MovedRows is sent without looking at its table. A table nothing wrote to, with nothing moving in
// it, costs next to nothing, whatever its size. In a table something wrote to, which spawning anything into it does,
// each row is checked against a hash of what it last sent. What stopped moving is sent once more when its previous
// transform catches up, in a tick: that row has no motion in it. Those tables' rows are cut into chunks and run as one
// parallel job. After the first frames it doesn't allocate (the arrays keep their capacity).
//
// What's sent past kRenderReachM from the origin is reported, and logged once. What the camera left that far behind
// since it was sent isn't: streaming has to have unloaded it long before.
//
// An entity leaves the renderer when it loses its MeshInstance or goes. Disabling it (flecs::Disabled) only stops
// updates: it stays drawn where it was.
void add_render_extract(flecs::world& ecs, flecs::entity phase, const RenderExtractDesc& desc);

// A line for the debug views of dev builds (physics' colliders), in world space: drawn over the scene, faint where
// something stands in front of it (render::SceneRenderer::set_debug_lines).
struct DebugLine {
    DVec3 from{0.0};
    DVec3 to{0.0};
    uint32_t color = 0xffffffffU; // rgba8, red in the low byte, as seen (sRGB)
    bool through = true;          // also drawn, faint, where something hides it
};

} // namespace de::world
