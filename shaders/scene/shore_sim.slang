// The surf, simulated (docs/design.md, Water; engine/render/scene/shore_sim.h): the shallow water equations on a grid of
// kSide x kSide cells kCellM across, a window around the camera that moves with it a whole cell at a time. The grid is
// stored round a torus: a cell's texel is its place in the world modulo kSide, so moving the window only makes the
// cells that came into it, and nothing is copied.
//
// Each cell holds the water's depth h and the ground's height b (both about the sea's level), and the velocity across
// two of its faces: u through the face toward +x, w through the face toward +z (a staggered grid). A step:
//   velocity_main: the velocities carried along by themselves (semi-Lagrangian: each face takes what was upstream of
//                  it), then pushed by the surface's slope (gravity). A face with no water on its upwind side, or
//                  between two dry cells, holds none.
//   height_main:   each cell's depth changed by what flows across its faces, carrying the upwind cell's depth.
// The shallow water equations hold where the water is shallow next to its waves' length: the surf zone and the beach.
// Out of them come waves slowing and steepening as the bottom rises, bending to meet the shore, breaking into bores
// (a jump the equations carry), and running up the beach and back (cells go wet and dry).
//
// The sea's waves come in at the window's edges: a band of cells round it is drawn toward the waves the water draws
// outside the window (shore_waves.slang: the same surf and swash, by the same formula), strongly at the edge and
// less and less inward, over the same stretch the water fades from those waves to the simulation's. So where one hands
// over to the other they're the same waves, and the simulation's own start as those. The band also swallows what
// comes back out. Deeper than kDeepM the ground is taken as kDeepM down: long waves feel only that much, and the step
// stays stable.
//
// Out where the swell hasn't broken yet the water is held to those waves too, everywhere in the window. The shallow
// water equations carry every wave at the one speed its depth gives, with nothing to hold a swell's shape: left to
// them, a swell steepens into a bore long before the depth would break it, and has spent itself by the time it
// reaches the beach. So the swell comes in as the formula has it, and the simulation's own water starts where that
// says it breaks: the bore from there, and its run up the beach, at the height a wave has when it breaks.
//
// And at the waterline the water's depth (not its flow) is drawn, gently, toward the formula's swash: how far up the
// beach each wave runs, and when. A bore that has crossed a wide, flat surf zone has little left to run up with; on a
// real beach most of the run-up is the slow surge of the sets, which these equations, fed single waves, don't make.
// So the run-up near the camera is as high and as timed as the one drawn far off, and where it goes on the ground,
// how it flows and what foam it leaves are the simulation's.

import common.bindless;
import scene.scene_common;
import scene.shore_map;
import scene.shore_waves;
import scene.terrain_common;

// Matches ShoreSimPush in engine/render/scene/shore_sim.cpp.
struct ShoreSimPush
{
    SceneFrame* frame;
    TerrainLevelInfo* levels;
    int2* nodes;     // per node: asset::TerrainNode
    uint* slots;     // per node: where its heights are, or kTerrainNoSlot
    uint* heights;
    uint state_in;   // storage: h, u, w, b
    uint state_out;
    uint surface;    // storage: the surface the water reads (surface_main)
    uint shore_material;
    uint flags;      // kFlagTerrain, kFlagFresh, kFlagPrevious
    uint finest_level;
    uint2 terrain_origin_steps;
    uint2 terrain_size_steps;
    int2 corner_cell;          // the window's corner, in cells of the world
    int2 previous_corner_cell; // last frame's: cells outside it came into the window
    float2 corner;             // the window's corner, relative to the render origin
    float sea_level_m;         // in the world
    float dt_s;                // a step's; for surface_main, the frame's steps' together
    float time_offset_s;       // this step's time, before the frame's
    float wave_amplitude_m;    // the swell's, as it comes in
    float wave_period_s;
};

[[vk::push_constant]]
ConstantBuffer<ShoreSimPush> push;

static const uint kSide = 512;       // kShoreSimSide
static const float kCellM = 0.5;     // kShoreSimCellM
static const uint kFlagTerrain = 1;
static const uint kFlagFresh = 2;
static const uint kFlagPrevious = 4; // last frame's surface is there to read
static const float kGravity = 9.81;
static const float kDeepM = 8.0;     // the ground is taken no deeper than this below the sea
static const float kDryM = 0.005;    // a cell holding less is dry
static const float kMostSpeedMps = 12.0;
// The band round the window that feeds the sea's waves in: wholly theirs out to kFeedFullM from the edge, free past
// kFeedNoneM (water.slang's kSimFadeFromM, kSimFadeToM: where it draws one, the other, and between).
static const float kFeedFullM = 18.0;
static const float kFeedNoneM = 48.0;
static const float kFeedPull = 0.06;      // a step's share of the way to them, at the most
static const float kFeedMostSpeedMps = 3.0;
static const float kSwellPull = 0.03;     // a step's share, where the swell is whole
static const float kSwellBroken = 0.5;    // how broken the surf is (0 to 1) where the water is all its own
static const float kSwashPull = 0.012;    // a step's share of the depth, up the beach and at the waterline

// --- Where cells are ----------------------------------------------------------------------------------------------------

// A cell of the window (0 to kSide - 1 each way) in the world, and its texel.
int2 world_cell(int2 local)
{
    return push.corner_cell + local;
}

uint2 texel_of(int2 local)
{
    const int2 cell = world_cell(local);
    return uint2(((cell % int(kSide)) + int(kSide)) % int(kSide));
}

// A cell's middle, relative to the render origin.
float2 cell_middle(int2 local)
{
    return push.corner + (float2(local) + 0.5) * kCellM;
}

// --- The ground ---------------------------------------------------------------------------------------------------------

// The ground's height in the world at a point (render-origin space), from the finest level of the terrain that's
// there; found false off the terrain, or before any of it is.
float terrain_height(float2 xz, out bool found)
{
    found = false;
    const float2 origin_on_map =
        float2(int2(push.frame.render_origin_steps.xz - push.terrain_origin_steps)) * kMetersPerRenderStep;
    const float2 on_map = xz + origin_on_map;
    for (int level = int(push.finest_level); level >= 0; --level)
    {
        const TerrainLevelInfo info = push.levels[level];
        const float2 at_samples = on_map / info.spacing_m;
        const int2 cell = int2(floor(at_samples));
        const int2 size_cells = int2(push.terrain_size_steps / info.spacing_steps);
        if (any(cell < 0) || any(cell >= size_cells))
        {
            return 0.0;
        }
        const uint2 node = uint2(cell) / kTerrainNodeQuads;
        const uint node_index = info.first_node + node.y * info.nodes_x + node.x;
        const uint slot = push.slots[node_index];
        if (slot == kTerrainNoSlot)
        {
            continue;
        }
        const int2 at = int2(uint2(cell) % kTerrainNodeQuads) + int2(int(kTerrainBlockBorder));
        const uint block_word = slot * kTerrainBlockWords;
        const float base = float(push.nodes[node_index].x);
        const float2 t = at_samples - float2(cell);
        const float h00 = block_sample(push.heights, block_word, at);
        const float h10 = block_sample(push.heights, block_word, at + int2(1, 0));
        const float h01 = block_sample(push.heights, block_word, at + int2(0, 1));
        const float h11 = block_sample(push.heights, block_word, at + int2(1, 1));
        found = true;
        return (base + lerp(lerp(h00, h10, t.x), lerp(h01, h11, t.x), t.y)) * info.step_m;
    }
    return 0.0;
}

// The ground's height about the sea's level at a cell, never deeper than kDeepM: from the terrain, else from the
// shore map.
float ground_at(int2 local)
{
    const float2 xz = cell_middle(local);
    if ((push.flags & kFlagTerrain) != 0u)
    {
        bool found;
        const float height_m = terrain_height(xz, found);
        if (found)
        {
            return max(height_m - push.sea_level_m, -kDeepM);
        }
    }
    const ShoreSample shore = shore_map_at(push.frame, push.shore_material, xz);
    return shore.signed_m < 0.0 ? shore.ground_m : max(-shore.depth_m, -kDeepM);
}

// --- The sea's waves, as they come in -----------------------------------------------------------------------------------

struct Incoming
{
    float height_m;   // the surface, about the sea's level
    float2 velocity;  // the water's, x and z
};

// The waves the water draws at a point (shore_waves.slang), where the ground is ground_m about the sea's level, and
// the flow under them: a long wave running in toward the land moves the water with it, as fast as its height over
// the depth times its own speed.
Swell sea_swell()
{
    Swell swell;
    swell.side = float2(-push.frame.wind.y, push.frame.wind.x);
    swell.period_s = push.wave_period_s;
    swell.height_m = push.wave_amplitude_m;
    return swell;
}

Incoming incoming_at(ShoreSample shore, float2 xz, float ground_m, float time_s)
{
    Incoming wave;
    wave.height_m = 0.0;
    wave.velocity = float2(0.0);
    if (!shore.on_map)
    {
        return wave;
    }
    const Swell swell = sea_swell();
    wave.height_m = shore_waves_surface(swell, xz, shore.distance_m, shore.signed_m, shore.depth_m, time_s);
    const float depth_m = max(-ground_m, 0.3);
    if (shore.signed_m > 0.0)
    {
        const float speed = wave.height_m * sqrt(kGravity / depth_m);
        wave.velocity = shore.to_land * clamp(speed, -kFeedMostSpeedMps, kFeedMostSpeedMps);
    }
    return wave;
}

// How strongly a cell is drawn toward the sea's waves, a step: 0 inside, rising toward the window's edge.
float sponge(int2 local)
{
    const float2 from_edge = (float2(min(local, int2(kSide) - 1 - local)) + 0.5) * kCellM;
    const float t = 1.0 - smoothstep(kFeedFullM, kFeedNoneM, min(from_edge.x, from_edge.y));
    return t * t * kFeedPull;
}

// --- The state ----------------------------------------------------------------------------------------------------------

float4 load_state(uint index, int2 local)
{
    return storage_2d(index)[texel_of(clamp(local, int2(0), int2(kSide) - 1))];
}

// Cells that came into the window this frame start at rest: the sea's level where the ground is below it, dry above.
// Every cell's ground is read again each frame: the terrain's finer levels stream in after a cell's first.
[shader("compute")]
[numthreads(8, 8, 1)]
void prepare_main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= kSide))
    {
        return;
    }
    const int2 local = int2(id.xy);
    const int2 cell = world_cell(local);
    const int2 previous_local = cell - push.previous_corner_cell;
    const bool new_cell = (push.flags & kFlagFresh) != 0u || any(previous_local < 0) || any(previous_local >= int(kSide));
    const float ground = ground_at(local);
    RWTexture2D<float4> state = storage_2d(push.state_in);
    const uint2 texel = texel_of(local);
    if (new_cell)
    {
        state[texel] = float4(max(-ground, 0.0), 0.0, 0.0, ground);
        return;
    }
    float4 cell_state = state[texel];
    // The ground moved under it (a finer level came in): the surface stays where it was.
    const float surface = cell_state.x > kDryM ? cell_state.x + cell_state.w : max(ground, 0.0);
    cell_state.x = max(surface - ground, 0.0);
    cell_state.w = ground;
    state[texel] = cell_state;
}

// --- velocity_main ------------------------------------------------------------------------------------------------------

// The velocity along x at a point (in cells: cell i spans i to i + 1), from the faces around it: face c, between cells
// c and c + 1 along x, is at (c.x + 1, c.y + 0.5).
float sample_u(float2 at)
{
    const float2 p = at - float2(1.0, 0.5);
    const int2 c = int2(floor(p));
    const float2 t = p - float2(c);
    const float a = load_state(push.state_in, c).y;
    const float b = load_state(push.state_in, c + int2(1, 0)).y;
    const float d = load_state(push.state_in, c + int2(0, 1)).y;
    const float e = load_state(push.state_in, c + int2(1, 1)).y;
    return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y);
}

// Along z: face c, between cells c and c + 1 along z, is at (c.x + 0.5, c.y + 1).
float sample_w(float2 at)
{
    const float2 p = at - float2(0.5, 1.0);
    const int2 c = int2(floor(p));
    const float2 t = p - float2(c);
    const float a = load_state(push.state_in, c).z;
    const float b = load_state(push.state_in, c + int2(1, 0)).z;
    const float d = load_state(push.state_in, c + int2(0, 1)).z;
    const float e = load_state(push.state_in, c + int2(1, 1)).z;
    return lerp(lerp(a, b, t.x), lerp(d, e, t.x), t.y);
}

// A face's new velocity: carried, then pushed by gravity down the surface's slope between its two cells (here and
// there), held to nothing where the water can't flow.
float face_velocity(float carried, float4 here, float4 there)
{
    const float eta_here = here.x + here.w;
    const float eta_there = there.x + there.w;
    float v = carried - kGravity * push.dt_s / kCellM * (eta_there - eta_here);
    const bool here_dry = here.x <= kDryM;
    const bool there_dry = there.x <= kDryM;
    // No water on the side it would come from: none flows. Nor between two dry cells.
    if ((here_dry && there_dry) || (here_dry && eta_there <= here.w) || (there_dry && eta_here <= there.w))
    {
        return 0.0;
    }
    return clamp(v, -kMostSpeedMps, kMostSpeedMps);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void velocity_main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= kSide))
    {
        return;
    }
    const int2 local = int2(id.xy);
    const float4 here = load_state(push.state_in, local);
    const float4 east = load_state(push.state_in, local + int2(1, 0));
    const float4 north = load_state(push.state_in, local + int2(0, 1));
    // Where each face's water was a step ago: back along its velocity, in cells.
    const float2 face_x = float2(local) + float2(1.0, 0.5);
    const float2 face_z = float2(local) + float2(0.5, 1.0);
    const float2 velocity_x = float2(here.y, 0.25 * (here.z + east.z + load_state(push.state_in, local + int2(0, -1)).z +
                                                      load_state(push.state_in, local + int2(1, -1)).z));
    const float2 velocity_z = float2(0.25 * (here.y + north.y + load_state(push.state_in, local + int2(-1, 0)).y +
                                             load_state(push.state_in, local + int2(-1, 1)).y), here.z);
    const float carried_u = sample_u(face_x - velocity_x * push.dt_s / kCellM);
    const float carried_w = sample_w(face_z - velocity_z * push.dt_s / kCellM);
    float u = local.x < int(kSide) - 1 ? face_velocity(carried_u, here, east) : 0.0;
    float w = local.y < int(kSide) - 1 ? face_velocity(carried_w, here, north) : 0.0;
    storage_2d(push.state_out)[texel_of(local)] = float4(here.x, u, w, here.w);
}

// --- height_main --------------------------------------------------------------------------------------------------------

// What flows across a face, per meter of it: its velocity times the depth on its upwind side.
float face_flow(float v, float4 behind, float4 ahead)
{
    return v * (v >= 0.0 ? behind.x : ahead.x);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void height_main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= kSide))
    {
        return;
    }
    const int2 local = int2(id.xy);
    const float4 here = load_state(push.state_out, local);
    const float4 east = load_state(push.state_out, local + int2(1, 0));
    const float4 north = load_state(push.state_out, local + int2(0, 1));
    const float4 west = load_state(push.state_out, local + int2(-1, 0));
    const float4 south = load_state(push.state_out, local + int2(0, -1));
    const float out_east = local.x < int(kSide) - 1 ? face_flow(here.y, here, east) : 0.0;
    const float in_west = local.x > 0 ? face_flow(west.y, west, here) : 0.0;
    const float out_north = local.y < int(kSide) - 1 ? face_flow(here.z, here, north) : 0.0;
    const float in_south = local.y > 0 ? face_flow(south.z, south, here) : 0.0;
    float h = here.x - push.dt_s / kCellM * (out_east - in_west + out_north - in_south);
    h = max(h, 0.0);
    // Round the window: toward the sea's waves, the depth and the flow.
    float2 flow = here.yz;
    const float2 xz = cell_middle(local);
    const ShoreSample shore = shore_map_at(push.frame, push.shore_material, xz);
    float pull = sponge(local);
    if (shore.on_map && shore.signed_m > 0.0)
    {
        // And where the swell is whole.
        const float broken = surf_shape(sea_swell(), shore.distance_m, shore.depth_m).broken;
        pull = max(pull, kSwellPull * (1.0 - smoothstep(0.0, kSwellBroken, broken)));
    }
    // The swash: the depth alone.
    const float swash_pull = shore.on_map && shore.signed_m > -kSwashReachM ? kSwashPull * swash_share(shore.signed_m) : 0.0;
    if (max(pull, swash_pull) > 0.0)
    {
        const Incoming wave = incoming_at(shore, xz, here.w, push.frame.wind_field.z - push.time_offset_s);
        h = lerp(h, max(wave.height_m - here.w, 0.0), max(pull, swash_pull));
        flow = lerp(flow, h > kDryM ? wave.velocity : float2(0.0), pull);
    }
    storage_2d(push.state_in)[texel_of(local)] = float4(h, flow, here.w);
}

// --- surface_main -------------------------------------------------------------------------------------------------------

// What the water's surface reads, filtered: the surface's height about the sea's level, the depth, and the foam on
// it. A dry cell beside water carries that water's surface on (the nearer water counting for more), so the water's
// edge is where that surface meets the ground itself, not a line of cells; any other is drawn well under its ground,
// which hides it.
static const float kUnderDryM = 0.3;
//
// The foam is made where the water breaks (a steep face, a thin sheet running fast up the beach, and the front of
// water moving onto dry ground: the white line a wave's run-up leads with, and leaves behind it as it drains),
// carried along by the flow (from where the cell's water was a frame ago, in last frame's surface) and dies away
// over kFoamLifeS.
static const float kFoamLifeS = 4.0;
static const float kFoamFillS = 0.25;     // how long breaking takes to whiten the water
static const float kBreakSlope = 0.22;    // the surface's slope where foam starts, and how much more makes it full
static const float kBreakSlopeRange = 0.25;

// The foam a cell held last frame; none if it wasn't in the window then.
float foam_before(int2 local)
{
    const int2 before = world_cell(local) - push.previous_corner_cell;
    if (any(local < 0) || any(local >= int(kSide)) || any(before < 0) || any(before >= int(kSide)))
    {
        return 0.0;
    }
    return load_texture(push.frame.shore_previous_surface, int2(texel_of(local))).z;
}

// At a point, in cells (a cell's middle at its number and a half).
float foam_before_at(float2 at)
{
    const float2 p = at - 0.5;
    const int2 c = int2(floor(p));
    const float2 t = p - float2(c);
    return lerp(lerp(foam_before(c), foam_before(c + int2(1, 0)), t.x),
                lerp(foam_before(c + int2(0, 1)), foam_before(c + int2(1, 1)), t.x), t.y);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void surface_main(uint3 id: SV_DispatchThreadID)
{
    if (any(id.xy >= kSide))
    {
        return;
    }
    const int2 local = int2(id.xy);
    const float4 here = load_state(push.state_in, local);
    const float4 west = load_state(push.state_in, local + int2(-1, 0));
    const float4 south = load_state(push.state_in, local + int2(0, -1));
    const float4 east = load_state(push.state_in, local + int2(1, 0));
    const float4 north = load_state(push.state_in, local + int2(0, 1));
    const bool wet = here.x > kDryM;
    float surface = here.x + here.w;
    if (!wet)
    {
        float sum = 0.0;
        float weight = 0.0;
        bool beside = false;
        for (int i = 0; i < 25; ++i)
        {
            const int2 offset = int2(i % 5 - 2, i / 5 - 2);
            const float4 near = load_state(push.state_in, local + offset);
            if (near.x > kDryM)
            {
                const float2 away = float2(offset);
                const float share = 1.0 / dot(away, away);
                sum += (near.x + near.w) * share;
                weight += share;
                beside = beside || all(abs(offset) <= 1);
            }
        }
        surface = beside ? sum / weight : here.w - kUnderDryM;
    }
    const float2 velocity = float2(0.5 * (here.y + west.y), 0.5 * (here.z + south.z));

    // The foam: what the flow brought, fading, and what breaks here.
    float foam = 0.0;
    if ((push.flags & kFlagPrevious) != 0u)
    {
        foam = foam_before_at(float2(local) + 0.5 - velocity * push.dt_s / kCellM) * exp(-push.dt_s / kFoamLifeS);
    }
    if (wet)
    {
        // The slope, from the wet cells beside it: a dry one's ground is no part of the water's face.
        const float eta = here.x + here.w;
        const float eta_east = east.x > kDryM ? east.x + east.w : eta;
        const float eta_west = west.x > kDryM ? west.x + west.w : eta;
        const float eta_north = north.x > kDryM ? north.x + north.w : eta;
        const float eta_south = south.x > kDryM ? south.x + south.w : eta;
        const float slope = length(float2(eta_east - eta_west, eta_north - eta_south)) / (2.0 * kCellM);
        const float face = saturate((slope - kBreakSlope) / kBreakSlopeRange) * smoothstep(0.03, 0.2, here.x);
        const float sheet = (1.0 - smoothstep(0.05, 0.25, here.x)) * smoothstep(0.8, 2.5, length(velocity));
        const bool at_front = east.x <= kDryM || west.x <= kDryM || north.x <= kDryM || south.x <= kDryM;
        const float front = at_front ? max(0.4, smoothstep(0.15, 0.6, length(velocity))) : 0.0;
        foam = min(foam + max(max(face, sheet * 0.45), front) * push.dt_s / kFoamFillS, 1.0);
    }
    storage_2d(push.surface)[texel_of(local)] = float4(surface, here.x, foam, 0.0);
}
