// Water, drawn after the light pass and before the fog (docs/design.md, Water): its surface onto the lit image, with
// depth and motion, so the fog and TAA take it as any surface. What's behind it is read from copies made just before
// (water_behind.slang): seen through it, bent a little by its slopes, dimmed by how much water the light crossed and
// replaced by the light the water itself scatters (clear in the shallows, its own deep color farther down), then the
// sky and the sun reflected on top as Fresnel says.
//
// The mesh: square levels of a grid around the camera, each twice the spacing of the one inside it (water.h), moved by
// the ocean's waves (ocean.slang): three tiles, summed. Toward a level's edge its vertices morph into the next level's,
// and so does how finely they read the waves, so neighbouring levels meet without a crack. The slopes, read per pixel,
// give the normal; how much of them the mips averaged away gives the roughness, so far off the sun's glitter spreads
// as it does over waves too small to see.

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
    uint padding;
    float2 tile_offset[3]; // where the render origin is in each tile, as a share of it
};

[[vk::push_constant]]
ConstantBuffer<WaterPush> push;

static const uint kLevelQuads = 96; // kWaterLevelQuads
static const uint kSide = kLevelQuads + 1;
static const uint kTiles = 3;                                 // kOceanCascades
static const float kTileM[3] = {640.0, 101.0, 17.3};          // kOceanTileM
static const float kTexels = 256.0;                           // kOceanSize
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
static const float3 kFoamColor = float3(0.85, 0.87, 0.88);

struct VertexOutput
{
    float4 position : SV_Position;
    float3 from_origin : FROM_ORIGIN; // the moved surface
    float2 grid : GRID;               // where it came from on the sea's plane, x and z: what the slopes are read at
    float4 jittered : JITTERED;       // as drawn, for points behind it from their depth
    float4 clip : CLIP;
    float4 previous_clip : PREVIOUS_CLIP;
};

struct WaterOutput
{
    float4 color : SV_Target0;
    float2 motion : SV_Target1;
};

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

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VulkanVertexID, uint level_id: SV_VulkanInstanceID)
{
    const WaterLevel level = push.levels[level_id];
    const float2 grid = float2(float(vertex_id % kSide), float(vertex_id / kSide)) - float(kLevelQuads / 2);
    // Toward the edge, every other vertex slides onto its even neighbour: the next level's vertices, and its edges.
    const float edge = max(abs(grid.x), abs(grid.y)) / float(kLevelQuads / 2);
    const float morph = level_id + 1 < 12 ? saturate((edge - kMorphFrom) / (kMorphTo - kMorphFrom)) : 0.0;
    const float2 odd = grid - 2.0 * floor(grid * 0.5);
    const float2 xz = level.center + (grid - odd * morph) * level.spacing_m;
    float3 moved = float3(0.0);
    float3 moved_before = float3(0.0);
    if (push.has_waves != 0u)
    {
        const float spacing = level.spacing_m * (1.0 + morph);
        moved = displacement(push.waves, xz, spacing);
        moved_before = displacement(push.previous_waves, xz, spacing);
    }
    const float4 world = float4(xz.x + moved.x, push.sea_level + moved.y, xz.y + moved.z, 1.0);
    const float4 before = float4(xz.x + moved_before.x, push.sea_level + moved_before.y, xz.y + moved_before.z, 1.0);
    VertexOutput output;
    output.position = mul_rows(push.frame.view_proj, world);
    output.jittered = output.position;
    output.clip = mul_rows(push.frame.unjittered_view_proj, world);
    output.previous_clip = mul_rows(push.frame.previous_view_proj, before);
    output.from_origin = world.xyz;
    output.grid = xz;
    return output;
}

[shader("fragment")]
WaterOutput fragment_main(VertexOutput input)
{
    SceneFrame* frame = push.frame;
    const int2 texel = int2(input.position.xy);
    const float3 p = input.from_origin;
    const float3 to_camera = frame.camera_position.xyz - p;
    const float3 v = normalize(to_camera);
    const float3 l = frame.sun_direction.xyz;

    // The slopes, summed over the tiles, and what the mips averaged away of them: the roughness that's left.
    float2 slope = float2(0.0);
    float variance = 0.0;
    float foam = 0.0;
    if (push.has_waves != 0u)
    {
        const Texture2DArray texture = texture_2d_array(push.slopes);
        const SamplerState filtered = sampler_state(kSamplerAnisotropicWrap);
        for (uint tile = 0; tile < kTiles; ++tile)
        {
            const float4 s = texture.Sample(filtered, tile_uv(input.grid, tile));
            slope += s.xy;
            variance += max(s.z - dot(s.xy, s.xy), 0.0);
            foam = max(foam, s.w);
        }
    }
    float3 n = normalize(float3(-slope.x, 1.0, -slope.y));
    if (v.y < 0.0)
    {
        n = -n; // from below
    }
    const float a = sqrt(kSmoothest * kSmoothest + variance); // GGX's alpha: its square is the slopes' variance

    // What's behind, and how much water the light crossed to reach the eye from it: down to it and back up along the
    // view. Nothing behind (past the land's edge): deep water. What's seen through is bent by the slopes, unless
    // that would reach for something in front of the water.
    const float behind_depth = load_texture(push.behind_depth, texel).r;
    float path_m = kFarPathM;
    float column_m = kMostColumnM;
    if (behind_depth > 0.0)
    {
        const float3 behind = position_from_depth(frame, input.jittered.xy / input.jittered.w, behind_depth);
        column_m = clamp(push.sea_level - behind.y, 0.0, kMostColumnM);
        path_m = length(behind - p) + column_m;
    }
    const int2 size = int2(texture_size(push.behind_color));
    const float distance_m = length(to_camera);
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

    // Foam where the surface folds, lit as a rough white surface.
    const float3 foam_light = frame.sky_color.rgb + sun * saturate(dot(n, l) * 0.6 + 0.4);
    color = lerp(color, kFoamColor * foam_light, saturate(foam));

    WaterOutput output;
    output.color = float4(min(color, kHalfMax), 1.0);
    output.motion = motion_between(input.clip, input.previous_clip);
    return output;
}
