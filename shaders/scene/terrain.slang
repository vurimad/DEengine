// Terrain into the G-buffer. With layers (asset/terrain_layers_format.h), each ground pixel finds what its height, its
// slope and the map's masks ask for, and blends the textures of at most three layers by their surfaces' own heights, so
// sand settles between stones rather than fading over them. With none, one plain look: rock where it's steep, snow up
// high, sand by the sea. Below the sea, a dark blue stands in for water either way.
//
// This runs on most of the screen, so it reads only what it uses: fields of the tables rather than whole entries (every
// word through a pointer is a memory read, per pixel), anisotropic filtering only for color, and past where a layer's
// bumps can be seen, one read for its color and nothing else.

import common.bindless;
import scene.grass_common;
import scene.scene_common;
import scene.terrain_common;
import scene.terrain_light;

// Matches GpuTerrainLayer in engine/render/scene/terrain.cpp.
struct TerrainLayer
{
    uint material;
    uint tile_steps;
    uint projection; // 0: from above; 1: from whichever two directions face the ground most
    uint mask;
    float4 height_m;
    float4 slope_cos;
    float mask_from;
    float mask_scale;
    float noise;
    float noise_large;
    float variation;
    float3 tint;
    uint detail; // 4: none
    float detail_from;
    float detail_scale;
    uint flags; // kLayerDetailCoverage
};

// asset::TerrainLayerFlags::DetailCoverage: the layer's detail channel says how much of each texel it covers.
static const uint kLayerDetailCoverage = 1u;

// Matches TerrainDrawPush in engine/render/scene/terrain.cpp.
struct TerrainPush
{
    SceneFrame* frame;
    TerrainDraw* draws;
    TerrainLevelInfo* levels;
    uint* heights;
    uint2 origin_steps;
    uint2 size_steps;
    uint cascade;
    uint show_levels; // tint by level, for looking at what the quadtree chose
    TerrainLayer* layers;
    GpuMaterial* materials;
    uint layer_count;
    uint masks_material;
    float blend;
    uint has_masks;
    Grass* grass; // null: none
    uint details_material;
    uint has_details;
    uint normals_material;
    uint has_normals;
    uint colors_material;
    uint has_colors;
};

[[vk::push_constant]]
ConstantBuffer<TerrainPush> push;

static const uint kMaxBlended = 3;
static const float kLeastWeight = 0.01;
// A layer's bumps and small marks fade out this many of its tiles away, or from kDetailLeastM if that's nearer: a tile
// is a few pixels across by then. Over half as far again they're gone.
static const float kDetailTiles = 30.0;
static const float kDetailLeastM = 120.0;
// From here the same texture at kFarScale times the size takes over, which keeps rows of the tile from showing.
static const uint kFarScale = 5;
static const float kFarFadeFromM = 15.0;
static const float kFarFadeToM = 70.0;
static const float kFarMostShare = 0.8;
// Height blending sharpens where layers meet. Over this range it gives way to a plain fade: far off there are no bumps
// to blend by, and a sharp line across a valley reads as a mistake.
static const float kSoftFromM = 40.0;
static const float kSoftToM = 300.0;
// The ground under grass: how far along a blade its color is what shows from afar, and over what range it goes from the
// dark between blades to that.
static const float kGrassSeenFromAfar = 0.62;
static const float kGroundToGrassFromM = 10.0;
static const float kGroundToGrassToM = 70.0;
// The finest mip of the ground's normal map the layers' rules go by: 8 texels, 8 m on a map at 1 m.
static const float kBroadMip = 3.0;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float3 from_origin : FROM_ORIGIN; // relative to the render origin, so it's exact near the camera
    float height_m : HEIGHT;
    float4 clip : CLIP;
    float4 previous_clip : PREVIOUS_CLIP;
    nointerpolation uint level : LEVEL;
};

struct GBufferOutput
{
    float4 gb0 : SV_Target0;
    float2 gb1 : SV_Target1;
    float4 gb2 : SV_Target2;
    float4 emissive : SV_Target3;
    float2 motion : SV_Target4;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VulkanVertexID, uint draw_id: SV_VulkanInstanceID)
{
    const TerrainDraw draw = push.draws[draw_id];
    const TerrainPoint point = terrain_point(vertex_id, draw, push.levels, push.heights, push.frame, push.origin_steps, push.size_steps, true);
    const float4 world = float4(point.position, 1.0);
    VertexOutput output;
    output.position = mul_rows(push.frame.view_proj, world);
    output.clip = mul_rows(push.frame.unjittered_view_proj, world);
    output.previous_clip = mul_rows(push.frame.previous_view_proj, world); // the ground doesn't move
    output.normal = point.normal;
    output.from_origin = point.position;
    output.height_m = point.height_m;
    output.level = draw.level_x_z >> 28;
    return output;
}

float3 plain_color(float height_m, float3 normal)
{
    const float flat_share = smoothstep(0.62, 0.86, normal.y);
    const float3 soil = lerp(float3(0.34, 0.30, 0.20), float3(0.16, 0.19, 0.08), smoothstep(1.0, 6.0, height_m));
    const float3 rock = float3(0.27, 0.25, 0.23);
    const float3 land = lerp(rock, soil, flat_share);
    const float snow = smoothstep(640.0, 760.0, height_m) * smoothstep(0.5, 0.8, normal.y);
    return lerp(land, float3(0.8, 0.82, 0.86), snow);
}

// --- Noise -----------------------------------------------------------------------------------------------------------

float hash(float2 at)
{
    float3 p = frac(float3(at.xyx) * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

// Smooth, 0 to 1, changing over about one unit.
float value_noise(float2 at)
{
    const float2 cell = floor(at);
    float2 t = at - cell;
    t = t * t * (3.0 - 2.0 * t);
    return lerp(lerp(hash(cell), hash(cell + float2(1.0, 0.0)), t.x), lerp(hash(cell + float2(0.0, 1.0)), hash(cell + float2(1.0, 1.0)), t.x), t.y);
}

// --- Painted ground ---------------------------------------------------------------------------------------------------

// The ground's color worked out per pixel, as terrain tools paint theirs (the look's "colors": masks over the whole map
// that tools/worldgen/colorize.py bakes, read here). A test: its colors and rules live here until a look carries them.
static const float kRidgeScaleM = 6.0; // the masks' red: 0.5 + meters above the land around / this
static const float3 kRockLow = float3(0.070, 0.062, 0.052);  // dark brown-grey low down
static const float3 kRockHigh = float3(0.085, 0.090, 0.102); // dark blue-grey high up
static const float3 kRockRust = float3(0.110, 0.075, 0.055); // iron-stained, here and there
static const float3 kRidge = float3(0.150, 0.148, 0.145);    // weathered crests
static const float3 kStreak = float3(0.035, 0.035, 0.032);   // wet rock where water runs
static const float3 kSediment = float3(0.170, 0.160, 0.140); // scree, moraine and fans
static const float3 kTussock = float3(0.160, 0.120, 0.060);  // low gentle ground
static const float3 kSnow = float3(0.850, 0.860, 0.880);

// Noise, 0 to 1, about size_m across, from the sides the ground faces as much as from above, so it doesn't stretch down
// a cliff; flat where it's finer than the pixel, so it doesn't shimmer.
float facing_noise(float3 p, float3 facing, float size_m, float footprint_m)
{
    const float n = facing.y * value_noise(p.xz / size_m) + facing.z * value_noise(p.xy / size_m) + facing.x * value_noise(p.zy / size_m);
    return lerp(0.5, n, 1.0 - smoothstep(0.3 * size_m, 0.8 * size_m, footprint_m));
}

// p: meters from the map's corner, and height; masks: the painted look's (r ridges, g drainage, b scree).
float3 paint_ground(float3 p, float3 normal, float4 masks, float footprint_m)
{
    float3 facing = abs(normal);
    facing *= facing;
    facing *= facing;
    facing /= facing.x + facing.y + facing.z;
    const float slope_deg = degrees(acos(saturate(normal.y)));
    const float ridge_m = (masks.r - 0.5) * kRidgeScaleM;
    const float n180 = facing_noise(p, facing, 180.0, footprint_m);
    const float n40 = facing_noise(p, facing, 40.0, footprint_m);
    const float n9 = facing_noise(p, facing, 9.0, footprint_m);
    const float n2 = facing_noise(p, facing, 2.0, footprint_m);
    const float n05 = facing_noise(p, facing, 0.5, footprint_m);

    // Rock: browner low, bluer high, rusty in patches, its brightness wandering at every size.
    float3 color = lerp(kRockLow, kRockHigh, smoothstep(700.0, 2600.0, p.y));
    color = lerp(color, kRockRust, 0.6 * smoothstep(0.55, 0.8, n180 * 0.7 + n40 * 0.3));
    color *= 0.8 + 0.4 * (0.6 * n40 + 0.4 * n9);
    // Crests weathered pale, hollows and cracks dark, steep faces darker.
    color = lerp(color, kRidge, 0.7 * smoothstep(0.3, 2.5, ridge_m));
    color *= 1.0 - 0.35 * smoothstep(0.3, 2.5, -ridge_m);
    color *= 1.0 - 0.25 * smoothstep(45.0, 70.0, slope_deg);
    // Channels: wet and dark where water runs down steep rock; sediment where it slows, and scree, on ground that holds it.
    const float channel = smoothstep(0.4, 0.65, masks.g);
    color = lerp(color, kStreak, 0.6 * channel * smoothstep(20.0, 35.0, slope_deg));
    const float holds = 1.0 - smoothstep(34.0, 42.0, slope_deg);
    color = lerp(color, kSediment * (0.85 + 0.3 * n9), max(smoothstep(0.3, 0.8, masks.b), 0.5 * channel) * holds);
    // Tussock on low gentle ground.
    color = lerp(color, kTussock * (0.8 + 0.4 * n9), (1.0 - smoothstep(1000.0, 1300.0, p.y)) * (1.0 - smoothstep(22.0, 32.0, slope_deg)));
    // Grain, as fine as the pixel lets it be.
    return color * (0.82 + 0.36 * (0.6 * n2 + 0.4 * n05));
}

// --- A layer's textures ----------------------------------------------------------------------------------------------

// What a layer's textures say about a point.
struct Surface
{
    float3 color;
    float3 normal; // in the world
    float occlusion;
    float roughness;
    float height; // of its bumps, 0 to 1
};

// The fields of a material that terrain reads.
struct LayerTextures
{
    uint color;
    uint normal;
    uint packed; // occlusion, roughness, height
    float3 color_factor;
    float normal_scale;
    float roughness;
};

// One way of laying a texture on the ground.
struct Plane
{
    float2 uv;
    float2 uv_far; // for the same texture at kFarScale times the size
    float2 uv_dx;
    float2 uv_dy;
    float2 place_m; // where on the plane, in the world: noise follows this, not the render origin
    float3 tangent;
    float3 bitangent;
};

// How far into a tile the origin lies: in whole steps, so it holds anywhere in the world.
float tile_share(uint origin_steps, uint tile_steps)
{
    const int tile = int(tile_steps);
    int rest = int(origin_steps) % tile;
    if (rest < 0)
    {
        rest += tile;
    }
    return float(rest) / float(tile);
}

// Where in the tile a point reads, so repeats don't line up: slow noise picks one of many offsets, and where the pick
// changes, two are blended over a narrow band. Most pixels read once.
struct TilePick
{
    float2 first;
    float2 second;
    float t;
};

TilePick pick_offsets(float2 place_m, float tile_m)
{
    const float pick = value_noise(place_m / (tile_m * 5.3)) * 8.0;
    const float which = floor(pick);
    TilePick offsets;
    offsets.first = sin(float2(3.0, 7.0) * which);
    offsets.second = sin(float2(3.0, 7.0) * (which + 1.0));
    offsets.t = smoothstep(0.38, 0.62, pick - which);
    return offsets;
}

Surface read_plane(LayerTextures textures, Plane plane, float2 offset, float3 normal, float detail)
{
    const float2 uv = plane.uv + offset;
    Surface surface;
    surface.color = texture_2d(textures.color).SampleGrad(sampler_state(kSamplerAnisotropicWrap), uv, plane.uv_dx, plane.uv_dy).rgb;
    const SamplerState wrap = sampler_state(kSamplerLinearWrap);
    const float3 packed = texture_2d(textures.packed).SampleGrad(wrap, uv, plane.uv_dx, plane.uv_dy).rgb;
    surface.occlusion = packed.r;
    surface.roughness = packed.g;
    surface.height = packed.b;
    float2 xy = texture_2d(textures.normal).SampleGrad(wrap, uv, plane.uv_dx, plane.uv_dy).xy * 2.0 - 1.0;
    xy *= textures.normal_scale * detail;
    const float z = sqrt(saturate(1.0 - dot(xy, xy)));
    const float3 tangent = normalize(plane.tangent - normal * dot(normal, plane.tangent));
    const float3 bitangent = normalize(plane.bitangent - normal * dot(normal, plane.bitangent));
    surface.normal = tangent * xy.x + bitangent * xy.y + normal * z;
    return surface;
}

Surface mix_surfaces(Surface a, Surface b, float t)
{
    Surface mixed;
    mixed.color = lerp(a.color, b.color, t);
    mixed.normal = lerp(a.normal, b.normal, t);
    mixed.occlusion = lerp(a.occlusion, b.occlusion, t);
    mixed.roughness = lerp(a.roughness, b.roughness, t);
    mixed.height = lerp(a.height, b.height, t);
    return mixed;
}

float3 read_far_color(LayerTextures textures, Plane plane, float tile_m)
{
    const SamplerState wrap = sampler_state(kSamplerAnisotropicWrap);
    const float2 uv_dx = plane.uv_dx / float(kFarScale);
    const float2 uv_dy = plane.uv_dy / float(kFarScale);
    const TilePick offsets = pick_offsets(plane.place_m, tile_m * float(kFarScale));
    float3 color = texture_2d(textures.color).SampleGrad(wrap, plane.uv_far + offsets.first, uv_dx, uv_dy).rgb;
    if (offsets.t > 0.0)
    {
        color = lerp(color, texture_2d(textures.color).SampleGrad(wrap, plane.uv_far + offsets.second, uv_dx, uv_dy).rgb, offsets.t);
    }
    return color;
}

// Near, or fading: every map, without its repeats showing. Far: only the color, much larger.
Surface read_untiled(LayerTextures textures, Plane plane, float tile_m, float3 normal, float detail, float far_share)
{
    Surface surface;
    if (detail <= 0.0)
    {
        surface.color = read_far_color(textures, plane, tile_m);
        surface.normal = normal;
        surface.occlusion = 1.0;
        surface.roughness = 0.9;
        surface.height = 0.5;
        return surface;
    }
    const TilePick offsets = pick_offsets(plane.place_m, tile_m);
    surface = read_plane(textures, plane, offsets.t >= 1.0 ? offsets.second : offsets.first, normal, detail);
    if (offsets.t > 0.0 && offsets.t < 1.0)
    {
        surface = mix_surfaces(surface, read_plane(textures, plane, offsets.second, normal, detail), offsets.t);
    }
    if (far_share > 0.0)
    {
        surface.color = lerp(surface.color, read_far_color(textures, plane, tile_m), far_share);
    }
    if (detail < 1.0)
    {
        // Toward what the far path says, so nothing jumps where it takes over.
        surface.occlusion = lerp(1.0, surface.occlusion, detail);
        surface.roughness = lerp(0.9, surface.roughness, detail);
        surface.height = lerp(0.5, surface.height, detail);
    }
    return surface;
}

// What a pixel knows that every layer needs.
struct Pixel
{
    float3 from_origin;
    float3 dx;
    float3 dy;
    float2 world_xz;
    float3 normal;
    float3 broad; // the ground's normal no finer than kBroadMip: what the layers' rules and projections go by
    float distance_m;
    float wander; // the slow noise brightness follows
};

// What a layer says about a point.
Surface read_layer(uint index, Pixel pixel)
{
    const uint material = push.layers[index].material;
    LayerTextures textures;
    textures.color = push.materials[material].base_color_texture;
    textures.normal = push.materials[material].normal_texture;
    textures.packed = push.materials[material].metallic_roughness_texture;
    textures.normal_scale = push.materials[material].normal_scale;

    const uint tile_steps = push.layers[index].tile_steps;
    const float tile_m = float(tile_steps) * kMetersPerRenderStep;
    const float detail_from_m = max(kDetailLeastM, tile_m * kDetailTiles);
    const float detail = 1.0 - saturate((pixel.distance_m - detail_from_m) / (detail_from_m * 0.5));
    const float far_share = max(smoothstep(kFarFadeFromM, kFarFadeToM, pixel.distance_m) * kFarMostShare, 1.0 - detail);

    const float per_m = 1.0 / tile_m;
    const float far_per_m = per_m / float(kFarScale);
    const uint3 origin = push.frame.render_origin_steps.xyz;
    const uint far_steps = tile_steps * kFarScale;
    const float2 shift_xz = detail > 0.0 ? float2(tile_share(origin.x, tile_steps), tile_share(origin.z, tile_steps)) : float2(0.0);
    const float2 far_shift_xz = far_share > 0.0 ? float2(tile_share(origin.x, far_steps), tile_share(origin.z, far_steps)) : float2(0.0);

    Plane top;
    top.uv = pixel.from_origin.xz * per_m + shift_xz;
    top.uv_far = pixel.from_origin.xz * far_per_m + far_shift_xz;
    top.uv_dx = pixel.dx.xz * per_m;
    top.uv_dy = pixel.dy.xz * per_m;
    top.place_m = pixel.world_xz;
    top.tangent = float3(1.0, 0.0, 0.0);
    top.bitangent = float3(0.0, 0.0, -1.0);

    Surface surface;
    if (push.layers[index].projection == 0)
    {
        surface = read_untiled(textures, top, tile_m, pixel.normal, detail, far_share);
    }
    else
    {
        // The two directions that face the ground most. From the sides, the texture's up stays up, so strata stay level.
        const float shift_y = detail > 0.0 ? tile_share(origin.y, tile_steps) : 0.0;
        const float far_shift_y = far_share > 0.0 ? tile_share(origin.y, far_steps) : 0.0;
        const float world_y = pixel.from_origin.y + float(int(origin.y)) * kMetersPerRenderStep;
        const float3 at = pixel.from_origin;
        Plane side_x;
        side_x.uv = float2(at.z * per_m + shift_xz.y, -(at.y * per_m + shift_y));
        side_x.uv_far = float2(at.z * far_per_m + far_shift_xz.y, -(at.y * far_per_m + far_shift_y));
        side_x.uv_dx = float2(pixel.dx.z, -pixel.dx.y) * per_m;
        side_x.uv_dy = float2(pixel.dy.z, -pixel.dy.y) * per_m;
        side_x.place_m = float2(pixel.world_xz.y, world_y);
        side_x.tangent = float3(0.0, 0.0, 1.0);
        side_x.bitangent = float3(0.0, 1.0, 0.0);
        Plane side_z;
        side_z.uv = float2(at.x * per_m + shift_xz.x, -(at.y * per_m + shift_y));
        side_z.uv_far = float2(at.x * far_per_m + far_shift_xz.x, -(at.y * far_per_m + far_shift_y));
        side_z.uv_dx = float2(pixel.dx.x, -pixel.dx.y) * per_m;
        side_z.uv_dy = float2(pixel.dy.x, -pixel.dy.y) * per_m;
        side_z.place_m = float2(pixel.world_xz.x, world_y);
        side_z.tangent = float3(1.0, 0.0, 0.0);
        side_z.bitangent = float3(0.0, 1.0, 0.0);

        float3 facing = abs(pixel.broad);
        facing *= facing;
        facing *= facing;
        Plane first = top;
        Plane second = side_x;
        float first_weight = facing.y;
        float second_weight = facing.x;
        if (facing.z < facing.x && facing.z < facing.y)
        {
            // Above and x.
        }
        else if (facing.x < facing.y)
        {
            second = side_z;
            second_weight = facing.z;
        }
        else
        {
            first = side_x;
            first_weight = facing.x;
            second = side_z;
            second_weight = facing.z;
        }
        const float t = second_weight / max(first_weight + second_weight, 1e-4);
        if (t < 0.02)
        {
            surface = read_untiled(textures, first, tile_m, pixel.normal, detail, far_share);
        }
        else if (t > 0.98)
        {
            surface = read_untiled(textures, second, tile_m, pixel.normal, detail, far_share);
        }
        else
        {
            surface = mix_surfaces(read_untiled(textures, first, tile_m, pixel.normal, detail, far_share), read_untiled(textures, second, tile_m, pixel.normal, detail, far_share), t);
        }
    }
    surface.color *= push.layers[index].tint * (1.0 + push.layers[index].variation * (pixel.wander * 2.0 - 1.0));
    return surface;
}

// --- Which layers ----------------------------------------------------------------------------------------------------

// How much of a layer its rules ask for here, 0 to 1. Noise only moves where the rules are fading. Cheap tests first, and
// out as soon as one says none: most layers don't apply to most of the land.
// details_width: how much each channel of the details changes across the pixel; details_far: 0 where a pixel is within a
// texel of them, rising to 1 where it spans two.
float layer_weight(uint index, float height_m, float up, float4 masks, float4 details, float4 details_width, float details_far,
                   float2 world_xz, inout float noise_near, float noise_far)
{
    float weight = 1.0;
    const uint mask = push.layers[index].mask;
    if (mask < 4)
    {
        weight = saturate((masks[mask] - push.layers[index].mask_from) * push.layers[index].mask_scale);
        if (weight <= 0.0)
        {
            return 0.0;
        }
    }
    const uint detail = push.layers[index].detail;
    if (detail < 4)
    {
        // The rule's ramp is at least a pixel wide, so a sharp one, as snow's edge is, draws a clean line at any distance,
        // not stairs up close or shimmer far off.
        const float scale = push.layers[index].detail_scale;
        const float ramp = 1.0 / abs(scale);
        const float past_middle = (details[detail] - push.layers[index].detail_from) * sign(scale) - 0.5 * ramp;
        float width = max(ramp, details_width[detail]);
        if ((push.layers[index].flags & kLayerDetailCoverage) != 0u)
        {
            // Far off, the share the coverage averages to over the pixel: the ramp spreads over all of 0 to 1. A rib of
            // rock thinner than the pixel then darkens it a little, as it would a photo, instead of vanishing.
            width = max(width, details_far);
        }
        weight *= saturate(0.5 + past_middle / width);
        if (weight <= 0.0)
        {
            return 0.0;
        }
    }
    const float4 h = push.layers[index].height_m;
    weight *= saturate((height_m - h.x) / max(h.y - h.x, 1e-3)) * (1.0 - saturate((height_m - h.z) / max(h.w - h.z, 1e-3)));
    if (weight <= 0.0)
    {
        return 0.0;
    }
    const float4 s = push.layers[index].slope_cos;
    weight *= saturate((s.x - up) / max(s.x - s.y, 1e-4)) * saturate((up - s.w) / max(s.z - s.w, 1e-4));
    if (weight <= 0.0 || weight >= 1.0)
    {
        return saturate(weight);
    }
    const float amount = push.layers[index].noise;
    if (amount <= 0.0)
    {
        return weight;
    }
    if (noise_near < 0.0)
    {
        noise_near = value_noise(world_xz / 31.0) * 0.65 + value_noise(world_xz / 7.0) * 0.35;
    }
    const float noise = lerp(noise_near, noise_far, push.layers[index].noise_large);
    return saturate(weight * (1.0 + amount) - amount * noise);
}

// normal: the ground's, as fine as the map has it; broad: the same, no finer than kBroadMip; mesh_normal: the mesh's,
// which the grass grows by. details: the map's details here, details_width how much they change across the pixel and
// details_far how far past a texel a pixel spans (layer_weight).
Surface layered_surface(VertexOutput input, float3 normal, float3 broad, float3 mesh_normal, float4 details, float4 details_width,
                        float details_far)
{
    const uint3 origin = push.frame.render_origin_steps.xyz;
    Pixel pixel;
    pixel.from_origin = input.from_origin;
    pixel.dx = ddx(input.from_origin);
    pixel.dy = ddy(input.from_origin);
    pixel.world_xz = input.from_origin.xz + float2(int2(origin.xz)) * kMetersPerRenderStep;
    pixel.normal = normal;
    pixel.broad = broad;
    pixel.distance_m = length(input.from_origin - push.frame.camera_position.xyz);
    const float noise_far = value_noise(pixel.world_xz / 310.0) * 0.6 + value_noise(pixel.world_xz / 97.0) * 0.4;
    pixel.wander = noise_far;

    // The masks lie over the whole map.
    const float2 map_uv = (pixel.world_xz - float2(push.origin_steps) * kMetersPerRenderStep) / (float2(push.size_steps) * kMetersPerRenderStep);
    float4 masks = float4(0.0);
    if (push.has_masks != 0)
    {
        const uint masks_texture = push.materials[push.masks_material].metallic_roughness_texture;
        masks = texture_2d(masks_texture).SampleLevel(sampler_state(kSamplerLinearClamp), map_uv, 0.0);
    }

    // From the top of the stack down, until something covers the rest.
    uint chosen[kMaxBlended];
    float weights[kMaxBlended];
    uint count = 0;
    float rest = 1.0;
    float noise_near = -1.0; // worked out if a layer needs it
    for (int i = int(push.layer_count) - 1; i >= 0 && count < kMaxBlended && rest > kLeastWeight; --i)
    {
        const float weight = rest * layer_weight(uint(i), input.height_m, broad.y, masks, details, details_width, details_far, pixel.world_xz, noise_near,
                                                 noise_far);
        if (weight > kLeastWeight)
        {
            chosen[count] = uint(i);
            weights[count] = weight;
            rest -= weight;
            ++count;
        }
    }

    Surface surfaces[kMaxBlended];
    float highest = -1.0;
    for (uint i = 0; i < count; ++i)
    {
        surfaces[i] = read_layer(chosen[i], pixel);
        highest = max(highest, surfaces[i].height * push.blend + weights[i]);
    }

    // Blended by height: what stands highest for how much of it is asked for shows, and the rest sinks away.
    Surface result;
    result.color = float3(0.0);
    result.normal = float3(0.0);
    result.occlusion = 0.0;
    result.roughness = 0.0;
    result.height = 0.0;
    float total = 0.0;
    const float soft = lerp(max(push.blend, 0.02), 1.0, smoothstep(kSoftFromM, kSoftToM, pixel.distance_m));
    const float floor_at = highest - soft;
    for (uint i = 0; i < count; ++i)
    {
        const float share = max(surfaces[i].height * push.blend + weights[i] - floor_at, 0.0);
        result.color += surfaces[i].color * share;
        result.normal += surfaces[i].normal * share;
        result.occlusion += surfaces[i].occlusion * share;
        result.roughness += surfaces[i].roughness * share;
        total += share;
    }
    if (total <= 0.0)
    {
        result.color = plain_color(input.height_m, broad);
        result.normal = normal;
        result.occlusion = 1.0;
        result.roughness = 0.92;
        return result;
    }
    result.color /= total;
    result.normal = normalize(result.normal);
    result.occlusion /= total;
    result.roughness /= total;

    // Where grass grows the ground takes its color: dark between the blades up close, so bare soil doesn't show through,
    // and as a blade's own color further off, so nothing marks where the last blades stop.
    if (push.grass != nullptr)
    {
        const float grown = grass_where(push.grass, input.height_m, mesh_normal.y, masks);
        if (grown > 0.0)
        {
            const float tone = 0.7 * grass_slow_tone(pixel.world_xz) + 0.3 * grass_noise(pixel.world_xz / 2.3);
            float3 base_color = push.grass.color_base;
            float3 tip_color = push.grass.color_tip;
            const uint other_mask = push.grass.other_mask;
            if (other_mask < 4u)
            {
                const float other = smoothstep(0.3, 0.7, masks[other_mask]);
                base_color = lerp(base_color, push.grass.other_base, other);
                tip_color = lerp(tip_color, push.grass.other_tip, other);
            }
            const float3 seen_from_afar = lerp(base_color, tip_color, kGrassSeenFromAfar) * (0.7 + 0.6 * tone);
            const float3 between_blades = base_color * 0.6;
            result.color = lerp(result.color, lerp(between_blades, seen_from_afar, smoothstep(kGroundToGrassFromM, kGroundToGrassToM, pixel.distance_m)), grown);
        }
    }
    return result;
}

[shader("fragment")]
GBufferOutput fragment_main(VertexOutput input)
{
    const float3 geometric = normalize(input.normal);
    // The ground's own normal, from the map where there is one: far off, where the mesh is coarse, it still has every
    // rib and gully the source had, mipped to what a pixel covers. Which layer goes where follows it no finer than
    // kBroadMip, so the wall of a crevasse in a glacier is shaded as one, not taken for a strip of rock.
    // Where on the maps that lie over the whole terrain, and how far that moves across the pixel: worked out here, where
    // every pixel of a quad runs, so the maps are read mipped to what a pixel covers.
    const float2 from_corner = input.from_origin.xz + float2(int2(push.frame.render_origin_steps.xz - push.origin_steps)) * kMetersPerRenderStep;
    const float2 uv = from_corner / (float2(push.size_steps) * kMetersPerRenderStep);
    const float2 uv_dx = ddx(uv);
    const float2 uv_dy = ddy(uv);
    float3 ground = geometric;
    float3 broad = geometric;
    if (push.has_normals != 0u)
    {
        const Texture2D normals = texture_2d(push.materials[push.normals_material].normal_texture);
        const float2 xz = normals.SampleGrad(sampler_state(kSamplerAnisotropicClamp), uv, uv_dx, uv_dy).xy * 2.0 - 1.0;
        ground = float3(xz.x, sqrt(saturate(1.0 - dot(xz, xz))), xz.y);
        const SamplerState clamped = sampler_state(kSamplerLinearClamp);
        const float2 broad_xz = normals.SampleLevel(clamped, uv, max(normals.CalculateLevelOfDetail(clamped, uv), kBroadMip)).xy * 2.0 - 1.0;
        broad = float3(broad_xz.x, sqrt(saturate(1.0 - dot(broad_xz, broad_xz))), broad_xz.y);
    }
    // The details: what the generator found in the land's shape (where snow lies, scree gathers...), which the layers'
    // rules read. How fast they change across the pixel keeps those rules' edges a pixel wide.
    float4 details = float4(0.0);
    float4 details_width = float4(0.0);
    float details_far = 0.0;
    if (push.has_details != 0u && push.layer_count != 0u)
    {
        const Texture2D details_texture = texture_2d(push.materials[push.details_material].metallic_roughness_texture);
        const SamplerState clamped = sampler_state(kSamplerLinearClamp);
        details = details_texture.SampleGrad(clamped, uv, uv_dx, uv_dy);
        details_width = fwidth(details);
        details_far = saturate(details_texture.CalculateLevelOfDetail(clamped, uv));
    }
    // The painted ground, where the look has it: its color per pixel, and the snow over it, its edge broken up close by
    // noise finer than the 2 m snow map, so it's crisp rather than stepped.
    float4 painted = float4(0.0);
    if (push.has_colors != 0u && push.layer_count != 0u)
    {
        const Texture2D paint_texture = texture_2d(push.materials[push.colors_material].metallic_roughness_texture);
        const float4 masks = paint_texture.SampleGrad(sampler_state(kSamplerLinearClamp), uv, uv_dx, uv_dy);
        const float3 p = float3(from_corner.x, input.height_m, from_corner.y);
        const float footprint_m = max(max(length(ddx(input.from_origin)), length(ddy(input.from_origin))), 1e-3);
        painted.rgb = paint_ground(p, ground, masks, footprint_m);
        painted.a = masks.a;
        float3 facing = abs(ground);
        facing *= facing;
        facing *= facing;
        facing /= facing.x + facing.y + facing.z;
        const float distance_m = length(input.from_origin - push.frame.camera_position.xyz);
        const float near = 1.0 - smoothstep(200.0, 900.0, distance_m);
        const float edge = (0.55 * facing_noise(p, facing, 3.0, footprint_m) + 0.45 * facing_noise(p, facing, 0.8, footprint_m) - 0.5) * 0.8 * near;
        const float cover = details.r + edge;
        const float snow = saturate(0.5 + (cover - 0.5) / max(max(fwidth(cover) * 1.5, details_far), 1e-3));
        painted.rgb = lerp(painted.rgb, kSnow * (0.96 + 0.08 * facing_noise(p, facing, 6.0, footprint_m)), snow);
    }
    float3 base;
    float3 n = ground;
    float occlusion = 1.0;
    float roughness = 0.92;
    if (input.height_m < 0.0)
    {
        base = float3(0.015, 0.04, 0.06);
    }
    else if (push.layer_count == 0)
    {
        base = plain_color(input.height_m, broad);
    }
    else
    {
        const Surface surface = layered_surface(input, ground, broad, geometric, details, details_width, details_far);
        base = surface.color;
        base = lerp(base, painted.rgb, painted.a);
        n = surface.normal;
        occlusion = surface.occlusion;
        roughness = surface.roughness;
    }
    if (push.show_levels != 0)
    {
        const float3 tints[kTerrainMaxLevels] = {float3(1.0, 0.25, 0.25), float3(1.0, 0.8, 0.2), float3(0.3, 0.9, 0.3), float3(0.25, 0.55, 1.0),
                                                 float3(0.8, 0.35, 0.95), float3(0.2, 0.9, 0.9), float3(1.0, 1.0, 1.0), float3(0.5, 0.5, 0.5)};
        base = tints[input.level] * (0.35 + 0.65 * saturate(geometric.y));
    }
    // How much sky the land around lets this place see: valleys and the feet of cliffs darken, at any distance.
    occlusion *= lerp(1.0, terrain_light(push.frame, input.from_origin).sky, 0.9);
    GBufferOutput output;
    output.gb0 = float4(base, input.height_m < 0.0 ? 0.0 : float(kLightingGroupGround) / 255.0);
    output.gb1 = octahedral_encode(n) * 0.5 + 0.5;
    output.gb2 = float4(roughness, 0.0, occlusion, 0.0);
    output.emissive = float4(0.0, 0.0, 0.0, 1.0);
    output.motion = motion_between(input.clip, input.previous_clip);
    return output;
}
