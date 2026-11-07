// Terrain into the G-buffer. One plain look until the ground has materials: rock where it's steep, snow up high, sand by
// the sea, a dark blue below it as a stand-in for water.

import scene.scene_common;
import scene.terrain_common;

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
};

[[vk::push_constant]]
ConstantBuffer<TerrainPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
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
    output.height_m = point.height_m;
    output.level = draw.level_x_z >> 28;
    return output;
}

float3 ground_color(float height_m, float3 normal)
{
    if (height_m < 0.0)
    {
        return float3(0.015, 0.04, 0.06);
    }
    const float flat_share = smoothstep(0.62, 0.86, normal.y);
    const float3 soil = lerp(float3(0.34, 0.30, 0.20), float3(0.16, 0.19, 0.08), smoothstep(1.0, 6.0, height_m));
    const float3 rock = float3(0.27, 0.25, 0.23);
    const float3 land = lerp(rock, soil, flat_share);
    const float snow = smoothstep(640.0, 760.0, height_m) * smoothstep(0.5, 0.8, normal.y);
    return lerp(land, float3(0.8, 0.82, 0.86), snow);
}

[shader("fragment")]
GBufferOutput fragment_main(VertexOutput input)
{
    const float3 n = normalize(input.normal);
    float3 base = ground_color(input.height_m, n);
    if (push.show_levels != 0)
    {
        const float3 tints[kTerrainMaxLevels] = {float3(1.0, 0.25, 0.25), float3(1.0, 0.8, 0.2), float3(0.3, 0.9, 0.3), float3(0.25, 0.55, 1.0),
                                                 float3(0.8, 0.35, 0.95), float3(0.2, 0.9, 0.9), float3(1.0, 1.0, 1.0), float3(0.5, 0.5, 0.5)};
        base = tints[input.level] * (0.35 + 0.65 * saturate(n.y));
    }
    GBufferOutput output;
    output.gb0 = float4(base, 0.0);
    output.gb1 = octahedral_encode(n) * 0.5 + 0.5;
    output.gb2 = float4(0.92, 0.0, 1.0, 0.0);
    output.emissive = float4(0.0, 0.0, 0.0, 1.0);
    output.motion = motion_between(input.clip, input.previous_clip);
    return output;
}
