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
// The shore (the sea's shore map, tools/worldgen/shore.py: distance to land, depth, the way to land): the waves feel the
// bottom, growing as it shallows and dying out at the waterline; surf, the swell's sets, runs in toward the land, its
// crests white where they break and against cliffs, leaving foam that thins seaward; the water's edge foams.

import common.bindless;
import scene.brdf;
import scene.scene_common;
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
    uint has_waves;
    uint waves;          // the ocean's, this frame (sampled arrays, a layer per tile)
    uint previous_waves; // last frame's, for motion
    uint slopes;
    uint shore_material; // the shore map's material slot, if has_shore
    float2 tile_offset[3]; // where the render origin is in each tile, as a share of it
    uint has_shore;
    float surf_period_s; // the surf's sets: the swell's period
    float surf_height_m; // their height, out where they start to feel the bottom
    uint padding;
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
static const float kDeepLight = 0.35; // of the light falling on the water, what its own color sends back up
static const float kFarPathM = 2000.0; // the path through the water where nothing's behind it
static const float kMostColumnM = 60.0;
static const float kSmoothest = 0.02;  // the sun's glitter on water as smooth as it gets (GGX alpha)
static const float kRefractPixels = 48.0; // how far the slopes bend what's seen through, at most
static const float3 kFoamColor = float3(0.82, 0.85, 0.86);
static const float3 kChurnedColor = float3(0.16, 0.36, 0.36); // water full of air: milky green-blue
static const float3 kCrestColor = float3(0.10, 0.45, 0.38); // light through a thin crest: green-blue

// The shore map (tools/worldgen/shore.py): distance in centimeters over two bytes, depth over kShoreDepthM.
static const float kShoreDepthM = 64.0;
// The surf: how long its waves are near the shore, where they start to rise out of the open sea's, how far crests
// still break white against a cliff, and how far the foam they leave drifts seaward.
static const float kSurfWavelengthM = 70.0;
static const float kSurfFromM = 200.0;
static const float kSurfFullM = 70.0;
static const float kWallFoamM = 18.0;
static const float kTrailFoamM = 90.0;
static const float kPeriodS = 256.0; // the wind's period, which wave_time wraps to
static const float kTwoPi = 6.28318531;
static const float kMostFoam = 0.8; // foam never covers all of a place: the sea shows through it

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

// --- Noise -----------------------------------------------------------------------------------------------------------

float hash(float2 at)
{
    float3 p = frac(float3(at.xyx) * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

float value_noise(float2 at)
{
    const float2 cell = floor(at);
    float2 t = at - cell;
    t = t * t * (3.0 - 2.0 * t);
    return lerp(lerp(hash(cell), hash(cell + float2(1.0, 0.0)), t.x), lerp(hash(cell + float2(0.0, 1.0)), hash(cell + float2(1.0, 1.0)), t.x), t.y);
}

// --- The shore -------------------------------------------------------------------------------------------------------

struct Shore
{
    float distance_m; // to land at the sea's level
    float depth_m;
    float2 to_land;   // the way there, x and z
    bool land;        // all land here, where the terrain covers the water
};

// Where there's no map, or past it: open, deep water.
Shore shore_at(float2 xz)
{
    Shore shore;
    shore.distance_m = 1e4;
    shore.depth_m = 1e3;
    shore.to_land = float2(0.0, -1.0);
    shore.land = false;
    if (push.has_shore == 0u)
    {
        return shore;
    }
    SceneFrame* frame = push.frame;
    const float2 from_corner = xz + float2(int2(frame.render_origin_steps.xz - frame.terrain_origin_steps)) * kMetersPerRenderStep;
    const float2 uv = from_corner / (float2(frame.terrain_size_steps) * kMetersPerRenderStep);
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return shore;
    }
    // Filtered here, from the four texels around: a distance split over two bytes can't be filtered as it's stored.
    const uint texture = frame.materials[push.shore_material].metallic_roughness_texture;
    const int2 size = int2(texture_size(texture));
    const float2 at = uv * float2(size) - 0.5;
    const int2 corner = int2(floor(at));
    const float2 t = at - float2(corner);
    float distance[4];
    float depth[4];
    for (int i = 0; i < 4; ++i)
    {
        const int2 texel = clamp(corner + int2(i & 1, i >> 1), int2(0), size - 1);
        const float4 map = load_texture(texture, texel);
        distance[i] = (round(map.r * 255.0) * 256.0 + round(map.g * 255.0)) * 0.01;
        depth[i] = map.b * kShoreDepthM;
    }
    shore.distance_m = lerp(lerp(distance[0], distance[1], t.x), lerp(distance[2], distance[3], t.x), t.y);
    shore.depth_m = lerp(lerp(depth[0], depth[1], t.x), lerp(depth[2], depth[3], t.x), t.y);
    // Toward land: down the distance's slope.
    const float2 slope = float2(lerp(distance[1] - distance[0], distance[3] - distance[2], t.y),
                                lerp(distance[2] - distance[0], distance[3] - distance[1], t.x));
    shore.to_land = dot(slope, slope) > 1e-8 ? -normalize(slope) : float2(0.0, -1.0);
    shore.land = max(max(depth[0], depth[1]), max(depth[2], depth[3])) <= 0.0 &&
                 max(max(distance[0], distance[1]), max(distance[2], distance[3])) <= 0.0;
    return shore;
}

// How the waves feel the bottom: higher as it shallows (Green's law, a quarter power of the depth), down to nothing at
// the waterline.
float shoaling(Shore shore)
{
    return clamp(pow(max(shore.depth_m, 0.5) / 20.0, -0.25), 1.0, 1.5) * saturate(shore.depth_m / 1.5);
}

struct Surf
{
    float height;     // m
    float push;       // m, toward land
    float2 slope;     // x, z
    float crest;      // 0 to 1: 1 on a crest
    float phase;      // in turns: 0 on a crest, rising seaward behind it
    float strength;   // 0 to 1: how much surf there is here
};

// The swell's sets running in toward the land: one long wave along the way to land, its crests sharpened, rising out
// of the open sea's from kSurfFromM in and breaking up against the shore; its time rounded to repeat with the wind's.
Surf surf_at(float2 xz, Shore shore, float time_s)
{
    Surf surf;
    surf.height = 0.0;
    surf.push = 0.0;
    surf.slope = float2(0.0);
    surf.crest = 0.0;
    surf.phase = 1.0;
    surf.strength = smoothstep(kSurfFromM, kSurfFullM, shore.distance_m);
    if (surf.strength <= 0.0 || push.surf_height_m <= 0.0)
    {
        return surf;
    }
    const float base = kTwoPi / kPeriodS;
    const float omega = max(round(kTwoPi / max(push.surf_period_s, 1.0) / base), 1.0) * base;
    const float k = kTwoPi / kSurfWavelengthM;
    // Along the shore the sets come and go, and arrive a little out of step.
    const float along = value_noise(xz / 260.0);
    const float phase = k * shore.distance_m + omega * time_s + along * 2.5;
    const float c = 0.5 + 0.5 * cos(phase);
    const float amplitude = push.surf_height_m * surf.strength * (0.55 + 0.45 * along);
    surf.height = amplitude * (c * c - 0.33);
    surf.push = amplitude * 0.6 * sin(phase);
    // d(height)/d(phase) times d(phase)/d(xz): the phase grows away from land.
    surf.slope = (-amplitude * c * sin(phase)) * k * -shore.to_land;
    surf.crest = c * c * c * c;
    surf.phase = frac(phase / kTwoPi);
    return surf;
}

// --- Waves -----------------------------------------------------------------------------------------------------------

// Where a point of the plane lies in a tile's texture.
float3 tile_uv(float2 xz, uint tile)
{
    return float3(xz / kTileM[tile] + push.tile_offset[tile], float(tile));
}

// The waves' displacement at a point of the plane, read as finely as vertices spacing_m apart can hold.
float3 displacement(uint waves, float2 xz, float spacing_m)
{
    const Texture2DArray texture = texture_2d_array(waves);
    const SamplerState wrap = sampler_state(kSamplerLinearWrap);
    float3 sum = float3(0.0);
    for (uint tile = 0; tile < kTiles; ++tile)
    {
        const float lod = log2(max(spacing_m * kTexels / kTileM[tile], 1.0));
        sum += texture.SampleLevel(wrap, tile_uv(xz, tile), lod).xyz;
    }
    return sum;
}

// Where a point of the plane is moved to, by the open sea's waves as the bottom lets them be and by the surf.
float3 moved(uint waves, float2 xz, float spacing_m, Shore shore, float time_s)
{
    float3 offset = float3(0.0);
    if (push.has_waves != 0u)
    {
        offset = displacement(waves, xz, spacing_m) * shoaling(shore);
    }
    const Surf surf = surf_at(xz, shore, time_s);
    offset.y += surf.height * saturate(shore.depth_m / 1.5);
    offset.xz += surf.push * shore.to_land * saturate(shore.depth_m / 1.5);
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
    const float2 xz = level.center + (grid - odd * morph) * level.spacing_m;
    const float spacing = level.spacing_m * (1.0 + morph);
    const Shore shore = shore_at(xz);
    const float3 now = moved(push.waves, xz, spacing, shore, push.frame.wind_field.z);
    const float3 before = moved(push.previous_waves, xz, spacing, shore, push.frame.wind_field.w);
    const float4 world = float4(xz.x + now.x, push.sea_level + now.y, xz.y + now.z, 1.0);
    const float4 last = float4(xz.x + before.x, push.sea_level + before.y, xz.y + before.z, 1.0);
    VertexOutput output;
    output.position = mul_rows(push.frame.view_proj, world);
    output.jittered = output.position;
    output.clip = mul_rows(push.frame.unjittered_view_proj, world);
    output.previous_clip = mul_rows(push.frame.previous_view_proj, last);
    output.from_origin = world.xyz;
    output.grid = xz;
    output.height = now.y;
    return output;
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
    const float footprint_m = max(length(ddx(input.grid)), length(ddy(input.grid)));
    const Shore shore = shore_at(input.grid);
    if (shore.land)
    {
        discard; // the terrain covers it: only through a crack in the ground would it show
    }
    const float shoal = shoaling(shore);
    const Surf surf = surf_at(input.grid, shore, time_s);

    // The slopes, summed over the tiles, and what the mips averaged away of them: the roughness that's left.
    float2 slope = float2(0.0);
    float variance = 0.0;
    float whitecaps = 0.0;
    if (push.has_waves != 0u)
    {
        const Texture2DArray texture = texture_2d_array(push.slopes);
        const SamplerState filtered = sampler_state(kSamplerAnisotropicWrap);
        for (uint tile = 0; tile < kTiles; ++tile)
        {
            const float4 s = texture.Sample(filtered, tile_uv(input.grid, tile));
            slope += s.xy;
            variance += max(s.z - dot(s.xy, s.xy), 0.0);
            whitecaps = max(whitecaps, s.w);
        }
    }
    slope = slope * shoal + surf.slope * saturate(shore.depth_m / 1.5);
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
    if (behind_depth > 0.0)
    {
        const float3 behind = position_from_depth(frame, input.jittered.xy / input.jittered.w, behind_depth);
        column_m = clamp(p.y - behind.y, 0.0, kMostColumnM);
        path_m = length(behind - p) + column_m;
    }
    const int2 size = int2(texture_size(push.behind_color));
    int2 bent = clamp(texel + int2(n.xz * kRefractPixels * saturate(path_m / 4.0) / (1.0 + distance_m / 30.0)),
                      int2(0), size - 1);
    if (load_texture(push.behind_depth, bent).r > input.position.z)
    {
        bent = texel;
    }
    const float3 behind_color = load_texture(push.behind_color, bent).rgb;
    const float3 extinction = kAbsorption + kScattering;
    const float3 through = exp(-extinction * path_m);

    // The sun on the water, with the shadows the land casts on it from its baked light map: that's height above the
    // ground below, and the water stands this much above it.
    const TerrainLight baked = terrain_light(frame, p);
    const float sun_seen = 1.0 - smoothstep(0.5, 4.0, baked.shadow_depth_m - column_m);
    const float3 sun = frame.sun_color.rgb * sun_seen;

    // The water's own color: what it scatters back of the sky's light and the sun's, more where it's deep.
    const float3 albedo = kScattering / extinction;
    const float3 falling = frame.sky_color.rgb + sun * saturate(l.y);
    const float3 body = albedo * falling * kDeepLight * (1.0 - through);
    const float3 refracted = behind_color * through + body;

    // The surface: the sky reflected (never from below the horizon: the waves would show it the ground), and the sun's
    // glitter.
    const float n_dot_v = max(dot(n, v), 1e-4);
    const float fresnel = f_schlick(float3(0.02), n_dot_v).x;
    float3 r = reflect(-v, n);
    r.y = abs(r.y);
    const float3 reflected = sky_gradient(frame, r);
    const float3 h = normalize(l + v);
    const float n_dot_l = saturate(dot(n, l));
    const float3 glitter = sun * d_ggx(saturate(dot(n, h)), a) * v_smith(n_dot_v, n_dot_l, a) *
                           f_schlick(float3(0.02), saturate(dot(v, h))) * n_dot_l;
    float3 color = lerp(refracted, reflected, fresnel) + glitter;

    // Light through a crest seen against the sun: on the high, tilted faces of the waves, where they're thin.
    const float backlit = pow(saturate(dot(-v, l)), 4.0) * saturate(dot(n, -l) * 0.5 + 0.5);
    const float crest_face = saturate(input.height / 1.5) * saturate(length(slope) * 5.0);
    color += kCrestColor * sun * backlit * crest_face * (1.0 - fresnel) * 0.3;

    // Foam: whitecaps where the open sea's waves fold (more where it shallows), surf white on its crests as it breaks
    // and against cliffs, the foam it leaves thinning seaward behind each crest, and the water's very edge.
    // The white water comes and goes in patches, by where it is, not by the shore map's directions, which turn from
    // texel to texel. Against the shore it's never solid: it bursts as a crest arrives and thins between them.
    const float patches = 0.45 + 0.9 * value_noise(input.grid / 90.0);
    const float breaking = saturate(1.0 - shore.depth_m / 4.0) * surf.crest;
    const float wall = exp(-shore.distance_m / kWallFoamM) * (0.25 + 0.5 * surf.crest) * surf.strength * patches;
    // Rising over a few meters behind each crest, not cut at it.
    const float trail = exp(-surf.phase * 3.0) * smoothstep(0.0, 0.06, surf.phase) * exp(-shore.distance_m / kTrailFoamM) *
                        surf.strength * patches;
    const float edge = 1.0 - smoothstep(0.05, 0.3, column_m);
    const float coverage = min(max(max(whitecaps * shoal, breaking * 0.8), max(wall + trail * 0.85, edge * 0.6)), kMostFoam);
    // Read in streaks along the wind, as foam lies on the sea, drifting with it.
    const float2 wind = frame.wind.xy;
    const float2 across = float2(-wind.y, wind.x);
    const float2 streaks = float2(dot(input.grid, wind) / 2.5, dot(input.grid, across)) - wind * time_s * 0.3;
    const float foam = foam_pattern(streaks, coverage, footprint_m, time_s);
    const float3 foam_light = frame.sky_color.rgb + sun * saturate(dot(n, l) * 0.6 + 0.4);
    // Under and around the foam the water is churned full of air: milky, pale green-blue, hiding what's below.
    const float churned = saturate(coverage * 1.4) * 0.6;
    color = lerp(color, kChurnedColor * foam_light, churned * (1.0 - fresnel));
    color = lerp(color, kFoamColor * foam_light, foam);

    WaterOutput output;
    output.color = float4(min(color, kHalfMax), 1.0);
    output.motion = motion_between(input.clip, input.previous_clip);
    return output;
}
