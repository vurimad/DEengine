// Water, drawn after the light pass and before the fog (docs/design.md, Water): its surface onto the lit image, with
// depth and motion, so the fog and TAA take it as any surface. What's behind it is read from copies made just before
// (water_behind.slang): seen through it, bent a little by its slopes, dimmed by how much water the light crossed and
// replaced by the light the water itself scatters (clear in the shallows, its own deep color farther down), then the
// sky and the sun reflected on top as Fresnel says, light through thin crests, and foam.
//
// The mesh: square levels of a grid around the camera, each twice the spacing of the one inside it (water.h), moved by
// the ocean's waves (ocean.slang): three tiles, summed. Toward a level's edge its vertices morph into the next level's,
// and so does how finely they read the waves, so neighbouring levels meet without a crack. The slopes, read per pixel,
// give the normal; how much of them the mips averaged away gives the roughness, so far off the sun's glitter spreads
// as it does over waves too small to see.
//
// The surf is the shore material's film where it carries one (surf_film.slang): a strip of beach simulated offline and
// played back along every shore. Without one, or with it turned off, it's the band surf and, near the camera, the
// simulated surf (shore_waves.slang, shore_sim.slang): an earlier attempt, kept for now to compare.
//
// The shore (the sea's shore map, tools/worldgen/shore.py: distance to land, depth, the way to land): the waves feel the
// bottom, growing as it shallows and dying out at the waterline; surf, the swell's sets, runs in toward the land, its
// crests white where they break and against cliffs, leaving foam that thins seaward; the water's edge foams.

import common.bindless;
import common.fog;
import scene.brdf;
import scene.forward;
import scene.light_boxes;
import scene.local_lights;
import scene.scene_common;
import scene.shore_map;
import scene.shore_waves;
import scene.surf_film;
import scene.sky;
import scene.terrain_light;

// Matches GpuWaterLevel in engine/render/scene/water.h.
struct WaterLevel
{
    float2 center; // x, z: relative to the render origin
    float spacing_m;
    uint hole;
};

// Matches WaterPush in engine/render/scene/scene_renderer.cpp.
struct WaterPush
{
    SceneFrame* frame;
    WaterLevel* levels;
    uint behind_color;
    uint behind_depth;
    float sea_level; // relative to the render origin
    uint atlas; // the sun's shadow atlas
    uint waves;          // the ocean's, this frame (sampled arrays, a layer per tile)
    uint previous_waves; // last frame's, for motion
    uint slopes;
    uint shore_material; // the shore map's material slot, if flags has kFlagShore
    float2 tile_offset[3]; // where the render origin is in each tile, as a share of it
    uint flags; // kFlagShore, kFlagBands
    float surf_period_s; // the surf's sets: the swell's period
    float surf_height_m; // their height, out where they start to feel the bottom
    uint foam_material;     // the sea's foam texture's material slot; 0: foam drawn from noise
    float2 swell_direction; // toward where the swell travels: its crests lie across it
    // A lake's or pond's outline (shape not kSea): its middle (x, z, relative to the render origin) and half extents
    // along its own axes, its own x in the world's, and how much of the sea's waves it has.
    float4 bounds;
    float2 axis;
    uint shape;
    float wave_scale;
};

[[vk::push_constant]]
ConstantBuffer<WaterPush> push;

static const uint kLevelQuads = 96; // kWaterLevelQuads
static const uint kSide = kLevelQuads + 1;
static const uint kLevels = 12;                       // kWaterLevels
static const uint kTiles = 3;                         // kOceanCascades
static const float kTileM[3] = {640.0, 101.0, 17.3};  // kOceanTileM
static const float kTexels = 256.0;                   // kOceanSize
// Toward a level's edge, from this share of its half-width on, its vertices morph into the next level's.
static const float kMorphFrom = 0.8;
static const float kMorphTo = 0.95;

// The sea's water, per meter (red, green, blue): absorption much as clear coastal sea water's (red goes in a few
// meters, blue lasts), and a little scattering from what's in it, which gives deep water its own color.
static const float3 kAbsorption = float3(0.46, 0.085, 0.055);
static const float3 kScattering = float3(0.010, 0.022, 0.026);
// A lake's: fresh water with what's washed into it, which takes the blue too: deep, it goes dark green-brown.
static const float3 kLakeAbsorption = float3(0.45, 0.16, 0.24);
static const float3 kLakeScattering = float3(0.010, 0.016, 0.010);
static const float kDeepLight = 0.35; // of the light falling on the water, what its own color sends back up
static const float kFarPathM = 2000.0; // the path through the water where nothing's behind it
static const float kMostColumnM = 60.0;
static const float kEdgeFadeM = 0.15; // the water fades into the ground under it over its last this deep
// Where the water meets the ground it doesn't end where the depth test says, but fades in over a band around it: from
// this far below the ground (the water is drawn this much nearer the camera than it is, so it isn't cut off there) to
// kEdgeFadeM above. The ground's cooked heights come in steps (a 64th of its spacing: 3 cm near, more farther off); on a
// gentle beach a hard cut traces them as stairs, and a band this wide blurs them away.
static const float kEdgeBelowM = 0.1;
static const float kSmoothest = 0.02;  // the sun's glitter on water as smooth as it gets (GGX alpha)
static const float kRefractPixels = 48.0; // how far the slopes bend what's seen through, at most
static const float3 kFoamColor = float3(0.82, 0.85, 0.86);
static const float3 kChurnedColor = float3(0.16, 0.36, 0.36); // water full of air: milky green-blue
static const float3 kCrestColor = float3(0.10, 0.45, 0.38); // light through a thin crest: green-blue

// The surf: how far crests still break white against a cliff, and how far the foam they leave drifts seaward.
static const float kWallFoamM = 18.0;
static const float kTrailFoamM = 90.0;
static const float kPeriodS = 256.0; // the wind's period, which wave_time wraps to
static const float kTwoPi = 6.28318531;
static const float kMostFoam = 0.8; // foam never covers all of a place: the sea shows through it

// WaterPush::flags: a shore map to read; the shore waves' bands drawn in color over the water (a debug view).
static const uint kFlagShore = 1;
static const uint kFlagBands = 2;
static const uint kFlagFilm = 4; // the surf from the shore material's film, where it carries one
static const uint kFlagWaves = 8; // the ocean's waves are there to read

// WaterPush::shape: the sea, or a lake or pond of this outline.
static const uint kSea = 0;
static const uint kRectangle = 1;
static const uint kEllipse = 2;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 from_origin : FROM_ORIGIN; // the moved surface
    float2 grid : GRID;               // where it came from on the sea's plane, x and z: what the slopes are read at
    float height : WAVE_HEIGHT;       // how far the waves lifted it
    float4 jittered : JITTERED;       // as drawn, for points behind it from their depth
    float4 clip : CLIP;
    float4 previous_clip : PREVIOUS_CLIP;
};

struct WaterOutput
{
    float4 color : SV_Target0;
    float2 motion : SV_Target1;
};

// --- The shore -------------------------------------------------------------------------------------------------------

struct Shore
{
    float distance_m; // to land at the sea's level (0 on land)
    float signed_m;   // to the waterline: out to sea positive, into the land negative
    float depth_m;
    float ground_m;   // the ground's height about the sea's level, finely, near it (kShoreGroundLowM to kShoreGroundHighM)
    float2 to_land;   // the way there, x and z
    bool land;        // land beyond where any swash reaches: no water to draw
};

// Where there's no map, or past it: open, deep water.
Shore shore_at(float2 xz)
{
    Shore shore;
    shore.distance_m = 1e4;
    shore.signed_m = 1e4;
    shore.depth_m = 1e3;
    shore.ground_m = -1e3;
    shore.to_land = float2(0.0, -1.0);
    shore.land = false;
    if ((push.flags & kFlagShore) == 0u)
    {
        return shore;
    }
    const ShoreSample map = shore_map_at(push.frame, push.shore_material, xz);
    shore.distance_m = map.distance_m;
    shore.signed_m = map.signed_m;
    shore.depth_m = map.depth_m;
    shore.ground_m = map.ground_m;
    shore.to_land = map.to_land;
    shore.land = shore.signed_m < -kSwashReachM;
    return shore;
}

// How the waves feel the bottom: higher as it shallows (Green's law, a quarter power of the depth), down to nothing at
// the waterline (by the distance to it, which the shore map holds finely; its depth is coarse).
float shoaling(Shore shore)
{
    return clamp(pow(max(shore.depth_m, 0.5) / 20.0, -0.25), 1.0, 1.5) * smoothstep(0.0, 12.0, shore.distance_m);
}

struct Surf
{
    float height;   // m
    float2 slope;   // x, z
    float crest;    // 0 to 1: the white lip of a broken wave's front
    float strength; // 0 to 1: how much surf there is here
    // The two waves over the point (ShoreWaves), and the foam each has here, 0 to 1: the newer's lip and what it leaves
    // behind it, and what's left of the older's under it.
    float newer_index;
    float newer_behind;
    float newer_foam;
    float older_index;
    float older_behind;
    float older_foam;
};


// --- The surf, from the bands (shore_waves.slang) -----------------------------------------------------------------------

// The swell that makes the surf here; none without a shore.
Swell sea_swell()
{
    Swell swell;
    swell.side = float2(-push.frame.wind.y, push.frame.wind.x);
    swell.period_s = push.surf_period_s;
    swell.height_m = (push.flags & kFlagShore) != 0u ? push.surf_height_m : 0.0;
    return swell;
}

// --- The surf, from its film (surf_film.slang) ---------------------------------------------------------------------------

// Whether the sea's surf is its film's: asked for, and the shore's material carries one. Then there's no band surf and
// no simulation: the film is the surf, the swash and their foam, at every distance.
bool film_surf()
{
    return (push.flags & kFlagFilm) != 0u && (push.flags & kFlagShore) != 0u && push.shape == kSea &&
           has_surf_film(push.frame, push.shore_material);
}

SurfFilm no_film()
{
    SurfFilm film;
    film.height_m = 0.0;
    film.depth_m = 0.0;
    film.thin = 0.0;
    film.foam = 0.0;
    film.slope = 0.0;
    film.face = 0.0;
    film.crest = 0.0;
    film.beach = 0.0;
    film.flow_mps = 0.0;
    return film;
}

// The film at a point of the shore. footprint_m: how far apart it's read (the mesh's spacing; 0 per pixel).
SurfFilm film_at(float2 xz, Shore shore, float time_s, float footprint_m)
{
    const float2 side = float2(-push.frame.wind.y, push.frame.wind.x);
    return surf_film_at(push.frame, push.shore_material, shore.signed_m, dot(xz, side), time_s, 2.0 * push.surf_height_m,
                        footprint_m);
}

static const float kFilmDryM = 0.0005; // the film's water thinner than this isn't drawn

// The way out to sea at a point, for what slides along it (the foam): up the slope of the distance to the waterline,
// taken over kSeawardReachM either way. The shore map's own way to land is the slope within one of its texels, which
// turns in a step at every texel's edge; a pattern slid along that breaks into blocks there.
static const float kSeawardReachM = 6.0;

float2 seaward_at(float2 xz, Shore shore)
{
    const float2 slope =
        float2(shore_signed_at(push.frame, push.shore_material, xz + float2(kSeawardReachM, 0.0)) -
                   shore_signed_at(push.frame, push.shore_material, xz - float2(kSeawardReachM, 0.0)),
               shore_signed_at(push.frame, push.shore_material, xz + float2(0.0, kSeawardReachM)) -
                   shore_signed_at(push.frame, push.shore_material, xz - float2(0.0, kSeawardReachM)));
    return dot(slope, slope) > 1e-6 ? normalize(slope) : -shore.to_land;
}
// Sand the run-up reaches stays wet between waves: darker, and it shines a little.
static const float3 kWetSand = float3(0.80, 0.76, 0.72);
static const float kWetSandShine = 0.35; // of the water's own reflection
static const float kTroughShare = 0.45;  // of the still water's depth, how far down the sea's own waves' troughs go
// A tall wave's front face is looked into, not through to the bottom: this much more water on the way, so it's the
// sea's own deep color, darker than the shallows around it. And its crest, thin, lets light through.
static const float kFilmFacePathM = 3.5;
static const float kFilmCrestGlow = 0.1;

// How wet the sand at a point of the beach is, 0 to 1: all of it where the waves run every time, drying over the
// meters up to where only the film's largest reach, unevenly, so it ends in no line. Fixed in place: it doesn't
// follow each wave.
float film_wet_sand(float2 xz, Shore shore)
{
    const float reach_m = kFilmRunUpM * surf_film_scale(2.0 * push.surf_height_m);
    const float inside_m = shore.signed_m + reach_m + (value_noise(xz / 6.0 + 13.7) - 0.5) * 5.0;
    return smoothstep(-3.0, 0.6 * reach_m, inside_m);
}

// The still water's depth at a point of the shore: the ground's, where the shore map has it finely, else the map's
// depth; the one eased into the other toward where the fine one ends (a meter down): they differ by up to the map's
// depth's step there, and switching would show as a line along the shore.
float still_depth(Shore shore)
{
    const float fine = smoothstep(kShoreGroundLowM + 0.05, kShoreGroundLowM + 0.45, shore.ground_m);
    return lerp(shore.depth_m, max(-shore.ground_m, 0.0), fine);
}

// How much of the open sea's own waves there is over the film's surf: all of them where it's a couple of meters deep,
// none in the last of it. The surf zone's waves are the film's; the sea's, left whole, are as high there as the water
// is deep, and their troughs would go under the sand. By the still water's depth: the ground's, where the shore map
// has it finely, else the map's depth.
float film_sea_share(Shore shore)
{
    return smoothstep(0.15, 2.2, still_depth(shore));
}

// How much of a point the older wave's tail still covers: all of it at first, gone at the tail's end.
float tail_cover(ShoreWave older)
{
    const float end = 2.0 * kTailLength;
    return 1.0 - smoothstep(end - 0.1, end, older.behind);
}

// The bands' debug view: each wave's sheet in a color of its own, its front's edge brighter, the newer over the older's
// tail, a little see-through so the tail shows.
static const float3 kWaveColors[4] = {float3(1.0, 0.45, 0.8), float3(0.35, 0.55, 1.0), float3(0.3, 1.0, 0.75),
                                      float3(0.75, 0.75, 0.75)};

float3 wave_debug_color(ShoreWave wave)
{
    const float3 color = kWaveColors[uint(wave.index + 4.0) % 4u];
    const float edge = 1.0 - smoothstep(0.0, 0.05, wave.behind);
    return lerp(color * 0.6, float3(1.0), edge * 0.7);
}

float3 shore_waves_debug(ShoreWaves waves)
{
    const float3 older = wave_debug_color(waves.older);
    const float3 newer = wave_debug_color(waves.newer);
    return lerp(newer, older, tail_cover(waves.older) * 0.45);
}

Surf no_surf()
{
    Surf surf;
    surf.height = 0.0;
    surf.slope = float2(0.0);
    surf.crest = 0.0;
    surf.newer_index = 0.0;
    surf.newer_behind = 0.0;
    surf.newer_foam = 0.0;
    surf.older_index = 0.0;
    surf.older_behind = 0.0;
    surf.older_foam = 0.0;
    surf.strength = 0.0;
    return surf;
}

Surf surf_at(float2 xz, Shore shore, float time_s)
{
    Surf surf;
    surf.height = 0.0;
    surf.slope = float2(0.0);
    surf.crest = 0.0;
    surf.newer_index = 0.0;
    surf.newer_behind = 0.0;
    surf.newer_foam = 0.0;
    surf.older_index = 0.0;
    surf.older_behind = 0.0;
    surf.older_foam = 0.0;
    const Swell swell = sea_swell();
    const SurfShape shape = surf_shape(swell, shore.distance_m, shore.depth_m);
    surf.strength = shape.strength;
    if (shore.signed_m < 0.0)
    {
        // Up the beach: no surf, only the swash, its foam read back from the waterline.
        const Swash swash = swash_at(swell, xz, shore.signed_m, time_s);
        surf.newer_index = swash.index;
        surf.newer_behind = -shore.signed_m / kBandWidthM;
        surf.newer_foam = swash.level_m > 0.0 ? swash.foam * swash.foam * 0.35 : 0.0; // clear, thin water: foam mostly fresh, and at its tip
        return surf;
    }
    if (surf.strength <= 0.0 || push.surf_height_m <= 0.0)
    {
        return surf;
    }
    const float amplitude_m = shape.amplitude_m;
    const float broken = shape.broken;
    const float2 side = swell.side;
    const float phase = band_phase(shore.distance_m, time_s, swell.period_s);
    const float along_m = dot(xz, side);
    const float2 warp = surf_warp(xz, shore.distance_m);
    surf.height = surf_height(phase + warp.x, along_m + warp.y, amplitude_m, broken);
    // The slopes, from the heights half a meter off along x and z: the distance to land there from its way, the
    // distortion there its own.
    const float step_m = 0.5;
    const float to_phase = 1.0 / kBandWidthM;
    const float2 warp_x = surf_warp(xz + float2(step_m, 0.0), shore.distance_m);
    const float2 warp_z = surf_warp(xz + float2(0.0, step_m), shore.distance_m);
    const float x = surf_height(phase - shore.to_land.x * step_m * to_phase + warp_x.x, along_m + side.x * step_m + warp_x.y,
                                amplitude_m, broken);
    const float z = surf_height(phase - shore.to_land.y * step_m * to_phase + warp_z.x, along_m + side.y * step_m + warp_z.y,
                                amplitude_m, broken);
    surf.slope = float2(x - surf.height, z - surf.height) / step_m;
    // Foam: the newer wave's lip and what it leaves behind, over the remains of the older one's.
    const ShoreWaves waves = shore_waves(swell, xz, shore.distance_m, time_s);
    const float newer_share = wave_share(waves.newer.index);
    const float older_share = wave_share(waves.older.index);
    surf.crest = broken * newer_share * exp(-waves.newer.behind / kLipWidth);
    const float left = exp(-waves.newer.behind * kTrailFall) * newer_share * 0.75;
    surf.newer_index = waves.newer.index;
    surf.newer_behind = waves.newer.behind;
    surf.newer_foam = max(surf.crest, broken * left);
    surf.older_index = waves.older.index;
    surf.older_behind = waves.older.behind;
    surf.older_foam = broken * exp(-waves.older.behind * kTrailFall) * older_share * 0.3;
    return surf;
}

// --- Waves -----------------------------------------------------------------------------------------------------------

// Where a point of the plane lies in a tile's texture.
float3 tile_uv(float2 xz, uint tile)
{
    return float3(xz / kTileM[tile] + push.tile_offset[tile], float(tile));
}

// How much of a tile's waves the water has: all of them at sea; on a lake, its share of the short ones, and of the
// long swell, which no lake raises, far less.
float tile_weight(uint tile)
{
    if (push.shape == kSea)
    {
        return 1.0;
    }
    return tile == 0 ? push.wave_scale * push.wave_scale : push.wave_scale;
}

// A point of a lake's plane in its own axes, from its middle.
float2 to_body(float2 xz)
{
    const float2 d = xz - push.bounds.xy;
    return float2(dot(d, push.axis), dot(d, float2(-push.axis.y, push.axis.x)));
}

float2 from_body(float2 local)
{
    return push.bounds.xy + local.x * push.axis + local.y * float2(-push.axis.y, push.axis.x);
}

// The waves' displacement at a point of the plane, read as finely as vertices spacing_m apart can hold.
float3 displacement(uint waves, float2 xz, float spacing_m, float swell_share)
{
    const Texture2DArray texture = texture_2d_array(waves);
    const SamplerState wrap = sampler_state(kSamplerLinearWrap);
    float3 sum = float3(0.0);
    for (uint tile = 0; tile < kTiles; ++tile)
    {
        const float lod = log2(max(spacing_m * kTexels / kTileM[tile], 1.0));
        sum += texture.SampleLevel(wrap, tile_uv(xz, tile), lod).xyz * tile_weight(tile) * (tile == 0 ? swell_share : 1.0);
    }
    return sum;
}

// --- The simulated surf ------------------------------------------------------------------------------------------------
//
// Near the camera the surf is simulated (shore_sim.slang): its surface replaces the band surf and the swash, and the
// ocean's swell tile, which it carries itself. Toward the window's edge, where the sea's waves are fed in, it hands
// back to them: from kSimFadeFromM in from the edge (past the band that feeds them in) to kSimFadeToM.
static const uint kSimSide = 512;  // kShoreSimSide
static const float kSimCellM = 0.5; // kShoreSimCellM
static const float kSimFadeFromM = 18.0;
static const float kSimFadeToM = 48.0;
static const float kSimSheetM = 0.04; // the simulated water is all there from this deep

struct SimSample
{
    float weight;   // 0: past the window, the band surf's; 1: the simulation's
    float height_m; // the surface, about the sea's level
    float depth_m;
    float foam;     // 0 to 1: what broke there, and what the flow brought
};

SimSample shore_sim_at(float2 xz, bool previous)
{
    SimSample sim;
    sim.weight = 0.0;
    sim.height_m = 0.0;
    sim.depth_m = 0.0;
    sim.foam = 0.0;
    SceneFrame* frame = push.frame;
    if (frame.shore_sim == 0u || push.shape != kSea)
    {
        return sim;
    }
    const float2 cells = (xz - frame.shore_corner) / kSimCellM;
    const float2 from_edge = min(cells, float(kSimSide) - cells) * kSimCellM;
    sim.weight = smoothstep(kSimFadeFromM, kSimFadeToM, min(from_edge.x, from_edge.y));
    if (sim.weight <= 0.0)
    {
        return sim;
    }
    // Round a torus: a cell's texel is its place in the world modulo the side, so the wrap sampler finds it.
    const float2 uv = (cells + float2(frame.shore_corner_cell)) / float(kSimSide);
    // Four taps half a cell off, each filtered: a cell and a half across, so the water's edge and its foam's don't
    // show the cells they're made of.
    const Texture2D<float4> texture = texture_2d(previous ? frame.shore_previous_surface : frame.shore_surface);
    const SamplerState filtered = sampler_state(kSamplerLinearWrap);
    const float2 half_cell = 0.5 / float(kSimSide);
    const float4 surface = 0.25 * (texture.SampleLevel(filtered, uv + float2(-half_cell.x, -half_cell.y), 0.0) +
                                   texture.SampleLevel(filtered, uv + float2(half_cell.x, -half_cell.y), 0.0) +
                                   texture.SampleLevel(filtered, uv + float2(-half_cell.x, half_cell.y), 0.0) +
                                   texture.SampleLevel(filtered, uv + float2(half_cell.x, half_cell.y), 0.0));
    sim.height_m = surface.x;
    sim.depth_m = surface.y;
    sim.foam = surface.z;
    return sim;
}

// The simulated surface's slope (x, z) at a point, from its neighbours a cell off.
float2 shore_sim_slope(float2 xz)
{
    const float x0 = shore_sim_at(xz - float2(kSimCellM, 0.0), false).height_m;
    const float x1 = shore_sim_at(xz + float2(kSimCellM, 0.0), false).height_m;
    const float z0 = shore_sim_at(xz - float2(0.0, kSimCellM), false).height_m;
    const float z1 = shore_sim_at(xz + float2(0.0, kSimCellM), false).height_m;
    return float2(x1 - x0, z1 - z0) / (2.0 * kSimCellM);
}

// Where a point of the plane is moved to, by the open sea's waves as the bottom lets them be and by the surf.
float3 moved(uint waves, float2 xz, float spacing_m, Shore shore, float time_s, bool previous)
{
    if (film_surf())
    {
        float3 offset = float3(0.0);
        if ((push.flags & kFlagWaves) != 0u)
        {
            offset = displacement(waves, xz, spacing_m, 1.0) * shoaling(shore) * film_sea_share(shore);
            // And a trough never goes deeper than a share of the water there, however high the swell: eased toward
            // that, so its bottom is still round. Else, over a couple of meters of water, a big swell's troughs reach
            // the seabed and it shows through in stains.
            const float most_m = kTroughShare * still_depth(shore);
            if (offset.y < 0.0)
            {
                offset.y = -most_m * tanh(-offset.y / max(most_m, 1e-3));
            }
        }
        // Out to sea, the film's surface. Where its water is thin (the last of the sea and the beach), that depth
        // over the ground as the shore map has it: the sheet lies on the real beach, and where it's dry the mesh
        // lies on the ground, with no step at the water's edge for the mesh to miss (the fragments end it, by the
        // film's own depth).
        const SurfFilm film = film_at(xz, shore, time_s, spacing_m);
        const bool ground_known = shore.ground_m > kShoreGroundLowM + 0.05 && shore.ground_m < kShoreGroundHighM - 0.05;
        // What's left of the sea's waves never takes more than half of thin water's depth.
        offset.y = lerp(offset.y, max(offset.y, -0.5 * film.depth_m), film.thin);
        // The seabed here needn't be the film's. Out to sea the surface is the film's own height about the sea's
        // level, its troughs no deeper than a share of the water that's really there; the film's depth over the
        // real ground is for its beach, where the two agree at the waterline. (Its depth laid on a seabed shallower
        // than its own would stand the water above the sea's level; on a deeper one, below it.)
        float height_m = film.height_m;
        const float most_m = kTroughShare * still_depth(shore);
        if (height_m < 0.0)
        {
            height_m = -most_m * tanh(-height_m / max(most_m, 1e-3));
        }
        offset.y += ground_known ? lerp(height_m, shore.ground_m + film.depth_m, film.thin * film.beach) : height_m;
        return offset;
    }
    const SimSample sim = shore_sim_at(xz, previous);
    float3 offset = float3(0.0);
    float3 short_waves = float3(0.0);
    if ((push.flags & kFlagWaves) != 0u)
    {
        offset = displacement(waves, xz, spacing_m, 1.0) * shoaling(shore);
        if (sim.weight > 0.0)
        {
            short_waves = displacement(waves, xz, spacing_m, 0.0) * shoaling(shore);
        }
    }
    if (sim.weight < 1.0)
    {
        const Surf surf = surf_at(xz, shore, time_s);
        offset.y += surf_shallows(surf.height, shore.distance_m);
        // The swash: up the beach, and over the last of the sea, which it rides up on.
        const Swash swash = swash_at(sea_swell(), xz, shore.signed_m, time_s);
        offset.y = max(offset.y, swash.level_m * swash_share(shore.signed_m));
    }
    if (sim.weight > 0.0)
    {
        // The simulation's surface, and the ocean's short waves on it where it's wet.
        const float wet = smoothstep(0.0, 0.3, sim.depth_m);
        const float3 simulated = float3(short_waves.xz * wet, sim.height_m + short_waves.y * wet).xzy;
        offset = lerp(offset, simulated, sim.weight);
    }
    return offset;
}

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VulkanVertexID, uint level_id: SV_VulkanInstanceID)
{
    const WaterLevel level = push.levels[level_id];
    const float2 grid = float2(float(vertex_id % kSide), float(vertex_id / kSide)) - float(kLevelQuads / 2);
    // Toward the edge, every other vertex slides onto its even neighbour: the next level's vertices, and its edges.
    const float edge = max(abs(grid.x), abs(grid.y)) / float(kLevelQuads / 2);
    const float morph = level_id + 1 < kLevels ? saturate((edge - kMorphFrom) / (kMorphTo - kMorphFrom)) : 0.0;
    const float2 odd = grid - 2.0 * floor(grid * 0.5);
    float2 xz = level.center + (grid - odd * morph) * level.spacing_m;
    if (push.shape != kSea)
    {
        // Held inside the lake's box: what's outside folds onto its edge. An ellipse's corners go in the fragments.
        xz = from_body(clamp(to_body(xz), -push.bounds.zw, push.bounds.zw));
    }
    const float spacing = level.spacing_m * (1.0 + morph);
    const Shore shore = shore_at(xz);
    const float3 now = moved(push.waves, xz, spacing, shore, push.frame.wind_field.z, false);
    const float3 before = moved(push.previous_waves, xz, spacing, shore, push.frame.wind_field.w, true);
    const float4 world = float4(xz.x + now.x, push.sea_level + now.y, xz.y + now.z, 1.0);
    const float4 last = float4(xz.x + before.x, push.sea_level + before.y, xz.y + before.z, 1.0);
    VertexOutput output;
    output.position = mul_rows(push.frame.view_proj, world);
    output.jittered = output.position;
    // Drawn nearer than it is by kEdgeBelowM of height along the view (reverse Z, no far plane: depth is near / w), so
    // where it's a little below the ground it still draws, for the fragment to fade.
    {
        const float3 to_camera = push.frame.camera_position.xyz - world.xyz;
        const float pull_m = kEdgeBelowM * length(to_camera) / max(abs(to_camera.y), 0.1 * length(to_camera));
        const float w = output.position.w;
        output.position.z *= w / max(w - pull_m, w * 0.5);
    }
    output.clip = mul_rows(push.frame.unjittered_view_proj, world);
    output.previous_clip = mul_rows(push.frame.previous_view_proj, last);
    output.from_origin = world.xyz;
    output.grid = xz;
    output.height = now.y;
    return output;
}

// --- The foam texture ------------------------------------------------------------------------------------------------

// The sea's foam texture (alpha: how much of each texel is foam), read twice: over kFoamTileM for the streaks and patches,
// over kFoamDetailTileM, turned, for what's in them, so neither repeat shows; stretched kFoamStretch times along a way
// (the wind for whitecaps, the crests for surf). Coverage 0 to 1 into how much of the pixel is white; shade: how bright
// the foam there is. The texture is taken in proportion to the coverage, never cut, so foam thins into the photo's own
// web rather than into flat white flecks.
static const float kFoamTileM = 18.0;
static const float kFoamDetailTileM = 5.0;
static const float kFoamStretch = 2.2;
static const float kFoamDensityTop = 0.45; // the texture's alpha this high is all foam

float foam_textured(float2 xz, float2 xz_dx, float2 xz_dy, float2 along, float coverage, float time_s,
                    out float shade)
{
    shade = 1.0;
    // Only where there's none at all: stopping a little above that is a line where the foam starts. Foam is many times
    // brighter than wet sand or dark water, so even a fiftieth of it, starting all at once, shows as an outline.
    if (coverage <= 0.0)
    {
        return 0.0;
    }
    const Texture2D texture = texture_2d(push.frame.materials[push.foam_material].base_color_texture);
    const SamplerState wrap = sampler_state(kSamplerAnisotropicWrap);
    const float2 across = float2(-along.y, along.x);
    // Into the texture: along the way, stretched; drifting slowly with the wind.
    const float2x2 to_local = float2x2(along.x / kFoamStretch, along.y / kFoamStretch, across.x, across.y);
    const float2 drift = push.frame.wind.xy * time_s * 0.25;
    const float2 local = mul(to_local, xz - drift);
    const float2 local_dx = mul(to_local, xz_dx);
    const float2 local_dy = mul(to_local, xz_dy);
    const float scale_a = 1.0 / kFoamTileM;
    const float scale_b = 1.0 / kFoamDetailTileM;
    const float2x2 turn = float2x2(0.62, -0.78, 0.78, 0.62);
    const float4 a = texture.SampleGrad(wrap, local * scale_a, local_dx * scale_a, local_dy * scale_a);
    const float4 b = texture.SampleGrad(wrap, mul(turn, local) * scale_b + 0.37, mul(turn, local_dx) * scale_b,
                                        mul(turn, local_dy) * scale_b);
    const float density = saturate((0.6 * a.a + 0.4 * b.a) / kFoamDensityTop);
    shade = 0.55 + 0.45 * density; // thin foam greyer, thick foam brightest: it has a body, not one flat white
    // The photo taken in proportion, never cut: where there's a lot of foam (a lip, a whitecap's heart) its thick
    // parts run together into whitewater with holes; as it thins, what's left is the photo's own web of lines, fainter
    // and fainter. Far off, its mips average it to the mean, as they should.
    return saturate(2.2 * density * coverage * (0.6 + 0.4 * coverage));
}

// The foam texture carried by the water's flow (meters a second, x and z): the surf's film's foam, which rides in behind
// a bore and is drawn back out by the backwash. The pattern is read twice, each time slid along the flow from a
// start of its own, for kFoamFlowCycleS at a time, the two half a cycle apart and each faded out as it nears its
// end (so the sliding never stretches it far, and nothing jumps when one starts again); each cycle from its own place
// in the texture, so it never comes back to where it was.
static const float kFoamFlowCycleS = 1.6;
static const float kFoamFlowCycles = 160.0; // in the wind's period, which the frame's time wraps to

float foam_flowing(float2 xz, float2 xz_dx, float2 xz_dy, float2 along, float2 flow, float coverage, float time_s,
                   out float shade)
{
    shade = 1.0;
    if (coverage <= 0.0)
    {
        return 0.0;
    }
    const Texture2D texture = texture_2d(push.frame.materials[push.foam_material].base_color_texture);
    const SamplerState wrap = sampler_state(kSamplerAnisotropicWrap);
    const float2 across = float2(-along.y, along.x);
    const float2x2 to_local = float2x2(along.x / kFoamStretch, along.y / kFoamStretch, across.x, across.y);
    const float2 local_dx = mul(to_local, xz_dx);
    const float2 local_dy = mul(to_local, xz_dy);
    const float scale_a = 1.0 / kFoamTileM;
    const float scale_b = 1.0 / kFoamDetailTileM;
    const float2x2 turn = float2x2(0.62, -0.78, 0.78, 0.62);
    float density = 0.0;
    float spread = 0.0;
    for (int i = 0; i < 2; ++i)
    {
        const float cycle = time_s / kFoamFlowCycleS + 0.5 * float(i);
        const float phase = frac(cycle);
        const float index = fmod(floor(cycle), kFoamFlowCycles);
        const float2 own = float2(hash(float2(index, 3.1 + float(i))), hash(float2(index, 17.7 + float(i)))) * 37.0;
        const float2 local = mul(to_local, xz - flow * (phase - 0.5) * kFoamFlowCycleS + own);
        const float4 a = texture.SampleGrad(wrap, local * scale_a, local_dx * scale_a, local_dy * scale_a);
        const float4 b = texture.SampleGrad(wrap, mul(turn, local) * scale_b + 0.37, mul(turn, local_dx) * scale_b,
                                            mul(turn, local_dy) * scale_b);
        const float weight = 1.0 - abs(2.0 * phase - 1.0);
        density += saturate((0.6 * a.a + 0.4 * b.a) / kFoamDensityTop) * weight;
        spread += weight * weight;
    }
    // Two patterns averaged are flatter than either: stretched back about the middle by as much as the average took,
    // or the foam would go soft and sharp again twice a cycle.
    density = saturate(0.5 + (density - 0.5) / sqrt(spread));
    shade = 0.55 + 0.45 * density;
    return saturate(2.2 * density * coverage * (0.6 + 0.4 * coverage));
}

// The foam texture as a wave of the surf carries it: read in the wave's own frame, along the shore and back from its
// front (meters), so the foam comes in with the wave and stays where it is on it; each wave from its own place in the
// texture, so no two carry the same foam. Stretched along the front, as foam left by a breaking crest lies.
static const float kWaveFoamStretch = 1.3;

float foam_in_wave(float2 local, float2 local_dx, float2 local_dy, float index, float coverage, out float shade)
{
    shade = 1.0;
    // Only where there's none at all: stopping a little above that is a line where the foam starts. Foam is many times
    // brighter than wet sand or dark water, so even a fiftieth of it, starting all at once, shows as an outline.
    if (coverage <= 0.0)
    {
        return 0.0;
    }
    const Texture2D texture = texture_2d(push.frame.materials[push.foam_material].base_color_texture);
    const SamplerState wrap = sampler_state(kSamplerAnisotropicWrap);
    const float2 stretch = float2(1.0 / kWaveFoamStretch, 1.0);
    const float2 own = float2(hash(float2(index, 11.3)), hash(float2(index, 29.1))) * 64.0;
    const float2 at = local * stretch + own;
    const float2 at_dx = local_dx * stretch;
    const float2 at_dy = local_dy * stretch;
    const float scale_a = 1.0 / kFoamTileM;
    const float scale_b = 1.0 / kFoamDetailTileM;
    const float2x2 turn = float2x2(0.62, -0.78, 0.78, 0.62);
    const float4 a = texture.SampleGrad(wrap, at * scale_a, at_dx * scale_a, at_dy * scale_a);
    const float4 b = texture.SampleGrad(wrap, mul(turn, at) * scale_b + 0.37, mul(turn, at_dx) * scale_b,
                                        mul(turn, at_dy) * scale_b);
    const float density = saturate((0.6 * a.a + 0.4 * b.a) / kFoamDensityTop);
    shade = 0.55 + 0.45 * density; // thin foam greyer, thick foam brightest: it has a body, not one flat white
    // The photo taken in proportion, never cut: where there's a lot of foam (a lip, a whitecap's heart) its thick
    // parts run together into whitewater with holes; as it thins, what's left is the photo's own web of lines, fainter
    // and fainter. Far off, its mips average it to the mean, as they should.
    return saturate(2.2 * density * coverage * (0.6 + 0.4 * coverage));
}

// --- Reflections -----------------------------------------------------------------------------------------------------

// Screen-space reflections: the reflected ray marched through the depth of what was drawn before the water, in steps
// that grow with distance (a cliff a kilometer off is found in a few dozen), then narrowed down where it went behind
// something. What's off the screen, or hidden behind something nearer, it can't see: there the sky stands in, faded
// toward it near the screen's edges so nothing pops.
static const uint kReflectionSteps = 40;
static const uint kReflectionRefine = 6;
static const float kReflectionGrowth = 1.22;
static const float kReflectionEdge = 0.12; // of the screen, either side, where reflections fade into the sky's

struct ScreenHit
{
    float3 color;
    float distance_m;
    float confidence; // 0: nothing found, the sky's
};

// Where a point lands on the screen, and how its depth compares with what's there: above 0, the point is behind it, by
// that many meters (view depth).
struct RayProbe
{
    float2 ndc;
    int2 texel;
    float behind_m;
    bool on_screen;
    bool sky;
};

RayProbe probe(SceneFrame* frame, float3 q, int2 size)
{
    RayProbe out;
    out.ndc = float2(0.0);
    out.texel = int2(0);
    out.behind_m = -1e9;
    out.on_screen = false;
    out.sky = false;
    const float4 clip = mul_rows(frame.view_proj, float4(q, 1.0));
    if (clip.w <= 1e-3)
    {
        return out;
    }
    out.ndc = clip.xy / clip.w;
    if (any(abs(out.ndc) > 1.0))
    {
        return out;
    }
    out.on_screen = true;
    const float2 uv = float2(out.ndc.x * 0.5 + 0.5, 0.5 - out.ndc.y * 0.5);
    out.texel = clamp(int2(uv * float2(size)), int2(0), size - 1);
    const float scene = load_texture(push.behind_depth, out.texel).r;
    if (scene <= 0.0)
    {
        out.sky = true; // nothing drawn there: the sky, which the ray can't hit
        return out;
    }
    // Reverse Z with an infinite far plane: depth is near / view depth, so the scene's view depth is the ray's scaled
    // by their depths' ratio.
    const float ray = clip.z / clip.w;
    const float scene_w = ray * clip.w / scene;
    out.behind_m = clip.w - scene_w;
    return out;
}

ScreenHit screen_reflection(SceneFrame* frame, float3 p, float3 r, float distance_m, int2 size)
{
    ScreenHit hit;
    hit.color = float3(0.0);
    hit.distance_m = 0.0;
    hit.confidence = 0.0;
    float t = max(0.5, distance_m * 0.004);
    float last_t = 0.0;
    for (uint i = 0; i < kReflectionSteps; ++i)
    {
        const RayProbe here = probe(frame, p + r * t, size);
        if (!here.on_screen)
        {
            break;
        }
        const float step_m = t - last_t;
        if (!here.sky && here.behind_m > 0.0)
        {
            // Gone behind something: narrow down where, then keep it only if it's the thing's surface, not far behind it.
            float lo = last_t;
            float hi = t;
            RayProbe found = here;
            for (uint k = 0; k < kReflectionRefine; ++k)
            {
                const float mid = 0.5 * (lo + hi);
                const RayProbe at = probe(frame, p + r * mid, size);
                if (at.on_screen && !at.sky && at.behind_m > 0.0)
                {
                    hi = mid;
                    found = at;
                }
                else
                {
                    lo = mid;
                }
            }
            const float thickness = max(0.6, (hi - lo) * 4.0 + 0.004 * t);
            if (found.behind_m < thickness)
            {
                const float2 edge = saturate((1.0 - abs(found.ndc)) / kReflectionEdge);
                hit.color = load_texture(push.behind_color, found.texel).rgb;
                hit.distance_m = hi;
                hit.confidence = edge.x * edge.y;
                return hit;
            }
            if (here.behind_m > step_m * 2.0 + 2.0)
            {
                break; // far behind something nearer: what the ray would hit is hidden from the camera
            }
        }
        last_t = t;
        t *= kReflectionGrowth;
    }
    return hit;
}

// --- Shading ---------------------------------------------------------------------------------------------------------

// Cell noise: the distance to the nearest of points scattered one to a cell, and to the next nearest. Their difference
// is small along the borders between cells: a web.
float2 cells(float2 at)
{
    const float2 cell = floor(at);
    const float2 inside = at - cell;
    float nearest = 8.0;
    float next = 8.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const float2 offset = float2(float(x), float(y));
            const float2 point = offset + float2(hash(cell + offset), hash(cell + offset + 17.31));
            const float d = length(point - inside);
            if (d < nearest)
            {
                next = nearest;
                nearest = d;
            }
            else if (d < next)
            {
                next = d;
            }
        }
    }
    return float2(nearest, next);
}

// Foam as foam is: soft patches of white, thicker where there's more, pocked with bubbles' holes of two sizes that
// grow and spread as it thins, so it opens up rather than ends at an edge. Coverage 0 to 1 into how much of the pixel is
// white; where the bubbles are finer than the pixel, their mean.
float foam_pattern(float2 xz, float coverage, float footprint_m, float time_s)
{
    const float detail = 1.0 - smoothstep(0.08, 0.4, footprint_m);
    if (coverage <= 0.03)
    {
        return 0.0;
    }
    const float mean = coverage * 0.75;
    if (detail <= 0.0)
    {
        return mean;
    }
    const float2 drift = float2(0.11, 0.06) * time_s;
    const float2 warp = float2(value_noise(xz / 2.3), value_noise(xz / 2.3 + 31.7)) - 0.5;
    const float2 at = xz + warp * 1.2 + drift;
    // The patches: noise at three sizes, thresholded softly by how much foam there is.
    const float body = 0.5 * value_noise(at / 2.2) + 0.3 * value_noise(at / 1.05 + 5.1) + 0.2 * value_noise(at / 0.5 + 9.7);
    const float density = smoothstep(1.0 - coverage - 0.2, 1.0 - coverage + 0.2, body);
    // The holes: the nearest bubble's middle, big and small, wider where the foam is thin.
    const float big = cells(at / 0.9).x;
    const float small = cells(at / 0.27 + 3.3).x;
    const float hole_big = 1.0 - smoothstep(0.18, 0.42, big + density * 0.3);
    const float hole_small = 1.0 - smoothstep(0.15, 0.35, small + density * 0.25);
    const float holes = max(hole_big, hole_small * 0.85);
    const float foam = density * (1.0 - holes * (1.0 - density * 0.5));
    return lerp(mean, foam, detail);
}

[shader("fragment")]
WaterOutput fragment_main(VertexOutput input)
{
    SceneFrame* frame = push.frame;
    const int2 texel = int2(input.position.xy);
    const float3 p = input.from_origin;
    const float3 to_camera = frame.camera_position.xyz - p;
    const float distance_m = length(to_camera);
    const float3 v = to_camera / distance_m;
    const float3 l = frame.sun_direction.xyz;
    const float time_s = frame.wind_field.z;
    const float2 grid_dx = ddx(input.grid);
    const float2 grid_dy = ddy(input.grid);
    const float footprint_m = max(length(grid_dx), length(grid_dy));
    if (push.shape == kEllipse)
    {
        const float2 inside = to_body(input.grid) / push.bounds.zw;
        if (dot(inside, inside) > 1.0)
        {
            discard;
        }
    }
    const Shore shore = shore_at(input.grid);
    if (shore.land)
    {
        // Under the land: the sand hides it, but where the ground dips below the sea's level inland it would show,
        // flat, with nothing for the surf's foam to follow. A texel or more in from the waterline, where the waves
        // have died out, so the real waterline isn't touched.
        discard;
    }
    const float shoal = shoaling(shore) * (film_surf() ? film_sea_share(shore) : 1.0);
    // Near the camera, the simulated surf in its place (and in the swell tile's, which it carries); the band surf only
    // where some of it shows.
    // Or all of it from its film: then neither.
    const bool film_mode = film_surf();
    SurfFilm film = no_film();
    float wet_sand = 0.0;
    if (film_mode)
    {
        film = film_at(input.grid, shore, time_s, 0.0);
        wet_sand = film_wet_sand(input.grid, shore) * film.thin;
        // There's water where the film has some: its mesh lies on the dry beach too. Where the sand is wet it's drawn
        // on, as wet sand.
        if (film.thin >= 1.0 && film.depth_m <= kFilmDryM && wet_sand <= 0.0)
        {
            discard;
        }
    }
    const SimSample sim = shore_sim_at(input.grid, false);
    const float band = film_mode ? 0.0 : 1.0 - sim.weight;
    const Surf surf = band > 0.0 ? surf_at(input.grid, shore, time_s) : no_surf();

    // The slopes, summed over the tiles, and what the mips averaged away of them: the roughness that's left.
    float2 slope = float2(0.0);
    float variance = 0.0;
    float whitecaps = 0.0;
    if ((push.flags & kFlagWaves) != 0u)
    {
        const Texture2DArray texture = texture_2d_array(push.slopes);
        const SamplerState filtered = sampler_state(kSamplerAnisotropicWrap);
        for (uint tile = 0; tile < kTiles; ++tile)
        {
            const float4 s = texture.Sample(filtered, tile_uv(input.grid, tile));
            const float weight = tile_weight(tile) * (tile == 0 ? band : 1.0);
            slope += s.xy * weight;
            variance += max(s.z - dot(s.xy, s.xy), 0.0) * weight * weight;
            whitecaps = max(whitecaps, s.w * weight);
        }
    }
    slope = slope * shoal + surf.slope * smoothstep(0.0, 8.0, shore.distance_m) * band;
    float2 sim_slope = float2(0.0);
    if (sim.weight > 0.0)
    {
        // Only where it's wet: past the water's edge the surface steps down under the ground, which isn't a slope of
        // the water.
        sim_slope = shore_sim_slope(input.grid) * smoothstep(0.05, 0.25, sim.depth_m);
        slope += sim_slope * sim.weight;
    }
    if (film_mode)
    {
        // The film's slope is across the shore: it rises seaward, away from the land. Less of it on a thin sheet, whose
        // front is a step a few centimeters high that would light up like a ridge.
        slope -= shore.to_land * film.slope * lerp(1.0, smoothstep(0.0, 0.08, film.depth_m), film.thin);
    }
    float3 n = normalize(float3(-slope.x, 1.0, -slope.y));
    if (v.y < 0.0)
    {
        n = -n; // from below
    }
    const float a = sqrt(kSmoothest * kSmoothest + variance * shoal * shoal); // GGX's alpha: its square, the variance

    // What's behind, and how much water the light crossed to reach the eye from it: down to it and back up along the
    // view. Nothing behind (past the land's edge): deep water. What's seen through is bent by the slopes, unless
    // that would reach for something in front of the water.
    const float behind_depth = load_texture(push.behind_depth, texel).r;
    float path_m = kFarPathM;
    float column_m = kMostColumnM;
    float over_ground_m = kMostColumnM; // the water's height over what's behind it: below 0 where it's under it
    if (behind_depth > 0.0)
    {
        const float3 behind = position_from_depth(frame, input.jittered.xy / input.jittered.w, behind_depth);
        over_ground_m = p.y - behind.y;
        // Where what's behind is the ground near the sea's level, its height from the shore map instead: smooth, where
        // the drawn ground's cooked heights come in steps a gentle beach turns into terraces.
        const float ground_y = push.sea_level + shore.ground_m;
        if (shore.ground_m > kShoreGroundLowM + 0.05 && shore.ground_m < kShoreGroundHighM - 0.05 &&
            abs(behind.y - ground_y) < 0.08)
        {
            over_ground_m = p.y - ground_y;
        }
        // Looking up at it from below its level, what's in front is lower than it: there, height can't tell; it's in
        // front only by depth.
        if (v.y < 0.0 && dot(behind - frame.camera_position.xyz, behind - frame.camera_position.xyz) < distance_m * distance_m)
        {
            discard;
        }
        // The film's thin water is as deep as the film has it: the mesh there lies on the shore map's ground, a
        // little off the drawn one either way, and more so far off, where the ground is drawn coarser.
        over_ground_m = lerp(over_ground_m, film.depth_m, film.thin);
        column_m = clamp(over_ground_m, 0.0, kMostColumnM);
        path_m = length(behind - p) + column_m;
    }
    path_m += kFilmFacePathM * film.face;
    const int2 size = int2(texture_size(push.behind_color));
    int2 bent = clamp(texel + int2(n.xz * kRefractPixels * saturate(path_m / 4.0) / (1.0 + distance_m / 30.0)),
                      int2(0), size - 1);
    if (load_texture(push.behind_depth, bent).r > input.position.z)
    {
        bent = texel;
    }
    // Wet sand is darker, under thin water as well as where the last wave left it.
    const float3 behind_color = load_texture(push.behind_color, bent).rgb * lerp(float3(1.0), kWetSand, wet_sand);
    const bool lake = push.shape != kSea;
    const float3 scattering = lake ? kLakeScattering : kScattering;
    const float3 extinction = (lake ? kLakeAbsorption : kAbsorption) + scattering;
    const float3 through = exp(-extinction * path_m);

    // The sun on the water, with the shadows the land casts on it from its baked light map: that's height above the
    // ground below, and the water stands this much above it.
    const TerrainLight baked = terrain_light(frame, p);
    // And those of what stands over it (a roof, a wall), from the sun's cascades.
    const float view_depth = dot(p - frame.camera_position.xyz, frame.camera_forward.xyz);
    const float sun_seen = (1.0 - smoothstep(0.5, 4.0, baked.shadow_depth_m - column_m)) *
                           sun_visibility(frame, push.atlas, p, float3(0.0, 1.0, 0.0), view_depth);
    const float3 sun = frame.sun_color.rgb * sun_seen;
    // The sky's light where the water stands: what a light box there holds of it (scene.light_boxes), which under an
    // open sky is the sky's own and in a hall only what gets in. For what it mirrors and finds nowhere on the screen,
    // the sky by the share of it the box sees that way.
    float3 mirrored = reflect(-v, n);
    mirrored.y = abs(mirrored.y);
    const BoxLight bounced = light_box_light(frame, p, float3(0.0, 1.0, 0.0), mirrored);
    const float open = open_sky(frame, p);
    const float3 sky_light = lerp(frame.sky_color.rgb * open, bounced.diffuse, bounced.weight);
    const float3 open_sky = hemisphere(frame, mirrored);
    const float sky_share = lerp(open,
                                 saturate(dot(bounced.mirror, float3(1.0)) / max(dot(open_sky, float3(1.0)), 1e-4)),
                                 bounced.weight);

    // The water's own color: what it scatters back of the sky's light and the sun's, more where it's deep.
    const float3 albedo = scattering / extinction;
    const float3 falling = sky_light + sun * saturate(l.y);
    const float3 body = albedo * falling * kDeepLight * (1.0 - through);
    const float3 refracted = behind_color * through + body;

    // The surface: the sky reflected (never from below the horizon: the waves would show it the ground), and the sun's
    // glitter.
    const float n_dot_v = max(dot(n, v), 1e-4);
    const float fresnel = f_schlick(float3(0.02), n_dot_v).x;
    float3 r = reflect(-v, n);
    r.y = abs(r.y);
    // What it reflects: the scene where the ray finds it on the screen, else the sky; each hazed by the fog along the
    // reflected ray as it would be seen along a ray of the camera's, since the fog pass hazes the water only as far as
    // its own surface.
    const ScreenHit seen = screen_reflection(frame, p, r, distance_m, size);
    const FogResult sky_fog = fog_between(frame.fog, p, r, 0.0, frame.fog.range.x, frame.sun_direction.xyz, frame.sun_color.rgb);
    const float3 sky_reflected = (sky_gradient(frame, r) * sky_fog.transmittance + sky_fog.in_scatter) * sky_share;
    float3 reflected = sky_reflected;
    if (seen.confidence > 0.0)
    {
        const FogResult fog = fog_between(frame.fog, p, r, 0.0, seen.distance_m, frame.sun_direction.xyz, frame.sun_color.rgb);
        reflected = lerp(sky_reflected, seen.color * fog.transmittance + fog.in_scatter, seen.confidence);
    }
    const float3 h = normalize(l + v);
    const float n_dot_l = saturate(dot(n, l));
    const float3 glitter = sun * d_ggx(saturate(dot(n, h)), a) * v_smith(n_dot_v, n_dot_l, a) *
                           f_schlick(float3(0.02), saturate(dot(v, h))) * n_dot_l;
    // Local lights glint on it as the sun does (a candle's flame on a pond): mirrored only, the water's own colour
    // takes nothing from them yet.
    LitSurface surface;
    surface.position = p;
    surface.normal = n;
    surface.to_camera = v;
    surface.albedo = float3(0.0);
    surface.f0 = float3(0.02);
    surface.a = a;
    surface.specular_share = 1.0;
    surface.translucency = 0.0;
    float3 color = lerp(refracted, reflected, fresnel) + glitter + local_lights(frame, surface).specular;

    // Light through a crest seen against the sun: on the high, tilted faces of the waves, where they're thin.
    const float backlit = pow(saturate(dot(-v, l)), 4.0) * saturate(dot(n, -l) * 0.5 + 0.5);
    const float crest_face = saturate(input.height / 1.5) * saturate(length(slope) * 5.0);
    color += kCrestColor * sun * backlit * crest_face * (1.0 - fresnel) * 0.3;

    // The water's very edge: in its last few centimeters it fades into what's under it, rather than ending in a line
    // against the ground (only where there is ground under it).
    if (behind_depth > 0.0)
    {
        // The simulated surf's thin sheets by the simulation's own depth, and sooner: a wave's run-up is a film a
        // few centimeters deep, which shows as the sky on it does.
        const float sheet = smoothstep(0.0, kSimSheetM, film_mode ? film.depth_m : sim.depth_m);
        const float cover = lerp(smoothstep(0.0, kEdgeFadeM, column_m), sheet, film_mode ? film.thin : sim.weight);
        color = lerp(behind_color, color, cover);
        // And wet sand shines, less than the water does: the water's edge is a change of depth, not of look.
        color += reflected * fresnel * kWetSandShine * wet_sand * (1.0 - cover);
    }
    // And below the ground, where it's only drawn for this, it fades out (foam and all, below).
    const float under_ground = 1.0 - smoothstep(-kEdgeBelowM, 0.0, over_ground_m);

    // Foam: whitecaps where the open sea's waves fold (more where it shallows), surf white on its crests as it breaks
    // and against cliffs, the foam it leaves thinning seaward behind each crest, and the water's very edge.
    // The white water comes and goes in patches, by where it is, not by the shore map's directions, which turn from
    // texel to texel. Against the shore it's never solid: it bursts as a crest arrives and thins between them.
    const float patches = 0.45 + 0.9 * value_noise(input.grid / 90.0);
    // Against a cliff: where it's still deep right by the land, not over a shelving beach.
    const float wall = exp(-shore.distance_m / kWallFoamM) * saturate(shore.depth_m / 4.0) * (0.25 + 0.5 * surf.crest) *
                       surf.strength * patches;
    // The surf's waves, each its own foam: the newer's lip solid, what they leave in patches, thinning seaward.
    const float surf_fade = surf.strength * exp(-shore.distance_m / kTrailFoamM);
    const float newer_coverage = min(surf.newer_foam * surf_fade * lerp(1.0, patches, 0.6), kMostFoam) * band;
    const float older_coverage = min(surf.older_foam * surf_fade * patches, kMostFoam) * band;
    // The simulation's: what broke, carried by its flow and thinning as it ages (shore_sim.slang, surface_main).
    const float sim_foam = sim.foam * sim.foam * smoothstep(0.0, 0.03, sim.depth_m) * sim.weight;
    // The film's: what its bores made, carried by its flow, up the beach and back.
    // Its amount squared: thick only where it's fresh, behind a bore's front, and lace soon after.
    // In patches a few meters across, so what a bore leaves is ragged, not a ribbon of one width. Fading in over the
    // first few centimeters of a sheet's depth, so its tip isn't a white line.
    const float film_wet = lerp(1.0, smoothstep(0.0, 0.05, film.depth_m), film.thin);
    const float film_foam = film.foam * film.foam * film_wet * lerp(0.45, 1.2, value_noise(input.grid / 9.0 + 71.3));
    // The water it's in is full of air for longer and farther than it's white: the foam's amount as it is, not squared.
    const float film_churn = film.foam * film_wet;
    // The water's very edge: at sea, foam the waves leave; on a lake, a thin broken line, as much as its waves.
    // Not where the surf is simulated: its foam is its own, and the shore map's distance there is too coarse an edge.
    const float sea_edge = (1.0 - smoothstep(0.01, 0.05, column_m)) * (1.0 - smoothstep(1.0, 4.0, shore.distance_m)) * band;
    const float edge = lake ? (1.0 - smoothstep(0.02, 0.08, column_m)) * push.wave_scale * patches : sea_edge;
    const float sea_coverage = min(whitecaps * shoal, kMostFoam);
    const float shore_coverage = min(max(max(wall, edge * 0.6), max(sim_foam, film_foam)), kMostFoam);
    const float coverage = max(max(sea_coverage, shore_coverage), max(newer_coverage, older_coverage));
    const float2 wind = frame.wind.xy;
    float foam = 0.0;
    float2 foam_flow = float2(0.0); // how the surf's film's foam is moving over the water, m/s
    float foam_shade = 1.0;
    if (push.foam_material != 0u)
    {
        // From the foam texture: whitecaps streaked along the wind, the surf's foam along its crests.
        const float2 swell = dot(push.swell_direction, push.swell_direction) > 1e-6 ? normalize(push.swell_direction) : wind;
        const float2 along_crests = float2(-swell.y, swell.x);
        float sea_shade;
        float shore_shade;
        const float on_sea = foam_textured(input.grid, grid_dx, grid_dy, wind, sea_coverage, time_s, sea_shade);
        // The surf's film's foam goes with its water.
        if (film_mode && shore_coverage > 0.0)
        {
            foam_flow = seaward_at(input.grid, shore) * film.flow_mps;
        }
        const float on_shore =
            film_mode ? foam_flowing(input.grid + 17.3, grid_dx, grid_dy, along_crests, foam_flow,
                                     shore_coverage, time_s, shore_shade)
                      : foam_textured(input.grid + 17.3, grid_dx, grid_dy, along_crests, shore_coverage, time_s, shore_shade);
        foam = max(on_sea, on_shore);
        foam_shade = on_sea > on_shore ? sea_shade : shore_shade;
        // The surf's two waves, each in its own frame: along the shore, and back from its front.
        const float2 side = float2(-frame.wind.y, frame.wind.x);
        const float along_m = dot(input.grid, side) + surf_warp(input.grid, shore.distance_m).y;
        const float2 local_dx = float2(dot(grid_dx, side), -dot(grid_dx, shore.to_land));
        const float2 local_dy = float2(dot(grid_dy, side), -dot(grid_dy, shore.to_land));
        float newer_shade;
        float older_shade;
        const float newer = foam_in_wave(float2(along_m, surf.newer_behind * kBandWidthM), local_dx, local_dy,
                                         surf.newer_index, newer_coverage, newer_shade);
        const float older = foam_in_wave(float2(along_m, surf.older_behind * kBandWidthM), local_dx, local_dy,
                                         surf.older_index, older_coverage, older_shade);
        // The newer over the older.
        const float waves = newer + older * (1.0 - newer);
        if (waves > foam)
        {
            foam_shade = newer > older ? newer_shade : older_shade;
            foam = waves;
        }
    }
    else
    {
        // From noise, read in streaks along the wind, drifting with it.
        const float2 across = float2(-wind.y, wind.x);
        const float2 streaks = float2(dot(input.grid, wind) / 2.5, dot(input.grid, across)) - wind * time_s * 0.3;
        foam = foam_pattern(streaks, coverage, footprint_m, time_s);
    }
    const float3 foam_light = sky_light + sun * saturate(dot(n, l) * 0.6 + 0.4);
    // Under and around the foam the water is churned full of air: milky, pale green-blue, hiding what's below.
    // And the film's surf zone stays stirred between its bores, long after their foam has gone: the more the shallower
    // it is, but not a sheet a few centimeters thin, which is clear over the sand.
    // Not on a tall wave's face: that's clear water rising, not stirred yet.
    const float stirred = film_mode ? 0.3 * (1.0 - film_sea_share(shore)) * smoothstep(0.05, 0.35, column_m) * (1.0 - film.face)
                                    : 0.0;
    const float churned = max(max(saturate(coverage * 1.4) * 0.6, stirred), 0.55 * film_churn);
    color += kCrestColor * falling * kFilmCrestGlow * film.crest * (1.0 - fresnel);
    color = lerp(color, kChurnedColor * foam_light, churned * (1.0 - fresnel));
    // The film's foam is thick where it's fresh and thinner, grayer as it ages.
    const float foam_body = film_mode ? lerp(0.78, 1.0, film.foam) : 1.0;
    color = lerp(color, kFoamColor * foam_light * foam_shade * foam_body, foam * foam_body);

    if ((push.flags & kFlagBands) != 0u && (push.flags & kFlagShore) != 0u && shore.distance_m < kBandsReachM && !film_mode)
    {
        float3 bands = shore_waves_debug(shore_waves(sea_swell(), input.grid, shore.distance_m, time_s));
        if (sim.weight > 0.0)
        {
            // The simulation's surface: blue below the sea's level, through white, to red a meter above; green: dry.
            const float h = clamp(sim.height_m, -1.0, 1.0);
            const float3 ramp = h < 0.0 ? lerp(float3(1.0), float3(0.1, 0.25, 1.0), -h) : lerp(float3(1.0), float3(1.0, 0.1, 0.05), h);
            bands = lerp(bands, sim.depth_m > 0.01 ? ramp : float3(0.1, 0.8, 0.2), sim.weight);
        }
        color = lerp(color, bands * (frame.sky_color.rgb + sun) * 0.5, 0.75);
    }

    if (behind_depth > 0.0)
    {
        color = lerp(color, load_texture(push.behind_color, texel).rgb, under_ground);
    }

    WaterOutput output;
    output.color = float4(min(color, kHalfMax), 1.0);
    // Where it's foam that's seen, the motion is the foam's: it slides over the water with the flow, and smoothing
    // over the frames by the water's own motion would smear it.
    float4 previous_clip = input.previous_clip;
    if (film_mode)
    {
        float frame_s = frame.wind_field.z - frame.wind_field.w;
        frame_s += frame_s < 0.0 ? kFoamFlowCycleS * kFoamFlowCycles : 0.0; // the time wrapped
        const float2 slid = foam_flow * frame_s;
        const float4 foam_was = input.previous_clip - mul_rows(frame.previous_view_proj, float4(slid.x, 0.0, slid.y, 0.0));
        previous_clip = lerp(input.previous_clip, foam_was, saturate(foam * 3.0));
    }
    output.motion = motion_between(input.clip, previous_clip);
    return output;
}
