// Terrain into a cascade of the shadow atlas, depth only: the same patches and morph as the camera sees, so the ground
// doesn't shadow itself where the two would differ.

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
    uint show_levels;
};

[[vk::push_constant]]
ConstantBuffer<TerrainPush> push;

[shader("vertex")]
float4 vertex_main(uint vertex_id: SV_VulkanVertexID, uint draw_id: SV_VulkanInstanceID) : SV_Position
{
    const TerrainPoint point = terrain_point(vertex_id, push.draws[draw_id], push.levels, push.heights, push.frame, push.origin_steps, push.size_steps, false);
    return cascade_clip(push.frame, push.cascade, point.position);
}

// Nothing to do, and no discard: early depth testing stays on.
[shader("fragment")]
void fragment_main()
{
}
