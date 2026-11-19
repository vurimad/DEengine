// The shore map (tools/worldgen/shore.py): what the water needs to know of the land under it and around it, baked over
// the whole terrain at a few meters a texel, as a material's metallic-roughness texture the world's sea names. Read by
// the water's surface (water.slang) and the surf's simulation (shore_sim.slang).
//   r, g: the signed distance to the waterline, in centimeters over two bytes, from kShoreSignedOffset: out to sea
//         positive, into the land negative
//   b: the water's depth, over kShoreDepthM
//   a: the ground's height about the sea's level, from kShoreGroundLowM to kShoreGroundHighM

import common.bindless;
import scene.scene_common;

public static const float kShoreDepthM = 64.0;
public static const float kShoreSignedOffset = 32768.0;
public static const float kShoreGroundLowM = -1.0;
public static const float kShoreGroundHighM = 3.0;

public struct ShoreSample
{
    public float distance_m; // to land at the sea's level (0 on land)
    public float signed_m;   // to the waterline: out to sea positive, into the land negative
    public float depth_m;
    public float ground_m;   // the ground's height about the sea's level, finely, near it
    public float2 to_land;   // the way there, x and z
    public bool on_map;      // false: past the map, or no map; open, deep water
};

// At a point of render-origin space (x, z), from the map in the material at slot material.
public ShoreSample shore_map_at(SceneFrame* frame, uint material, float2 xz)
{
    ShoreSample shore;
    shore.distance_m = 1e4;
    shore.signed_m = 1e4;
    shore.depth_m = 1e3;
    shore.ground_m = -1e3;
    shore.to_land = float2(0.0, -1.0);
    shore.on_map = false;
    const float2 from_corner = xz + float2(int2(frame.render_origin_steps.xz - frame.terrain_origin_steps)) * kMetersPerRenderStep;
    const float2 uv = from_corner / (float2(frame.terrain_size_steps) * kMetersPerRenderStep);
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return shore;
    }
    // Filtered here, from the four texels around: a distance split over two bytes can't be filtered as it's stored.
    const uint texture = frame.materials[material].metallic_roughness_texture;
    const int2 size = int2(texture_size(texture));
    const float2 at = uv * float2(size) - 0.5;
    const int2 corner = int2(floor(at));
    const float2 t = at - float2(corner);
    float distance[4];
    float depth[4];
    float ground[4];
    for (int i = 0; i < 4; ++i)
    {
        const int2 texel = clamp(corner + int2(i & 1, i >> 1), int2(0), size - 1);
        const float4 map = load_texture(texture, texel);
        distance[i] = (round(map.r * 255.0) * 256.0 + round(map.g * 255.0) - kShoreSignedOffset) * 0.01;
        depth[i] = map.b * kShoreDepthM;
        ground[i] = lerp(kShoreGroundLowM, kShoreGroundHighM, map.a);
    }
    shore.signed_m = lerp(lerp(distance[0], distance[1], t.x), lerp(distance[2], distance[3], t.x), t.y);
    shore.distance_m = max(shore.signed_m, 0.0);
    shore.depth_m = lerp(lerp(depth[0], depth[1], t.x), lerp(depth[2], depth[3], t.x), t.y);
    shore.ground_m = lerp(lerp(ground[0], ground[1], t.x), lerp(ground[2], ground[3], t.x), t.y);
    // Toward land: down the distance's slope.
    const float2 slope = float2(lerp(distance[1] - distance[0], distance[3] - distance[2], t.y),
                                lerp(distance[2] - distance[0], distance[3] - distance[1], t.x));
    shore.to_land = dot(slope, slope) > 1e-8 ? -normalize(slope) : float2(0.0, -1.0);
    shore.on_map = true;
    return shore;
}

// The signed distance to the waterline alone (shore_map_at's signed_m), for where it's wanted at several points; far
// out to sea off the map.
public float shore_signed_at(SceneFrame* frame, uint material, float2 xz)
{
    const float2 from_corner = xz + float2(int2(frame.render_origin_steps.xz - frame.terrain_origin_steps)) * kMetersPerRenderStep;
    const float2 uv = from_corner / (float2(frame.terrain_size_steps) * kMetersPerRenderStep);
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return 1e4;
    }
    const uint texture = frame.materials[material].metallic_roughness_texture;
    const int2 size = int2(texture_size(texture));
    const float2 at = uv * float2(size) - 0.5;
    const int2 corner = int2(floor(at));
    const float2 t = at - float2(corner);
    float distance[4];
    for (int i = 0; i < 4; ++i)
    {
        const float4 map = load_texture(texture, clamp(corner + int2(i & 1, i >> 1), int2(0), size - 1));
        distance[i] = (round(map.r * 255.0) * 256.0 + round(map.g * 255.0) - kShoreSignedOffset) * 0.01;
    }
    return lerp(lerp(distance[0], distance[1], t.x), lerp(distance[2], distance[3], t.x), t.y);
}
