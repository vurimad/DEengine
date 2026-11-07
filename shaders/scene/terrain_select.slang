// Which terrain patches to draw (docs/design.md, Terrain & vegetation). Each one decides for itself, from a flat table of
// what every patch of a level reaches: it's drawn if it's near enough for its level, its children aren't, and its heights
// are on the GPU. So the choice needs no walk down a tree, and whatever has loaded, what's drawn is one whole surface: a
// patch only gives way to children that are there. The choice is the same in every view, so shadows and the camera agree
// on the ground; each view then keeps what's in it.

import scene.scene_common;
import scene.terrain_common;

// Matches TerrainSelectPush in engine/render/scene/terrain.cpp.
struct TerrainSelectPush
{
    SceneFrame* frame;
    GpuCullView* views;
    TerrainLevelInfo* levels;
    float2* patches;  // per patch: the lowest and highest it reaches, in meters
    int2* nodes;      // per node: asset::TerrainNode
    uint* slots;      // per node: where its heights are, or kTerrainNoSlot
    TerrainDraw* draws; // list_capacity per view
    uint* args;       // per view: a DrawIndexedIndirect's five words
    uint2 origin_steps;
    float2 size_m;
    uint level;
    uint level_count;
    uint list_capacity;
};

[[vk::push_constant]]
ConstantBuffer<TerrainSelectPush> push;

static const uint kArgsWords = 5;
static const uint kArgsInstanceCount = 1;

float box_distance(float3 point, float3 low, float3 high)
{
    return length(max(max(low - point, point - high), float3(0.0)));
}

bool box_in_view(GpuCullView* view, float3 center, float3 half_size)
{
    for (uint p = 0; p < view.plane_count; ++p)
    {
        const float3 normal = view.planes[p].xyz;
        if (dot(normal, center) + view.planes[p].w < -dot(abs(normal), half_size))
        {
            return false;
        }
    }
    return true;
}

// Off the render origin, in whole steps, so it holds anywhere in the world.
float2 corner_of(uint2 patch, uint spacing_steps)
{
    const uint2 steps = push.origin_steps + patch * (kTerrainPatchQuads * spacing_steps);
    return float2(int2(steps - push.frame.render_origin_steps.xz)) * kMetersPerRenderStep;
}

uint patch_index(TerrainLevelInfo info, uint2 patch)
{
    return info.first_patch + patch.y * (info.nodes_x * 4u) + patch.x;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const TerrainLevelInfo info = push.levels[push.level];
    const uint2 patch = thread.xy;
    const float size_m = info.spacing_m * float(kTerrainPatchQuads);
    const uint2 node = patch / 4u;
    // Past the grid (a dispatch rounds up), or wholly past the edge of the map, there's nothing.
    bool drawn = patch.x < info.nodes_x * 4u && patch.y < info.nodes_z * 4u && float(patch.x) * size_m < push.size_m.x &&
                 float(patch.y) * size_m < push.size_m.y;
    uint slot = kTerrainNoSlot;
    if (drawn)
    {
        slot = push.slots[info.first_node + node.y * info.nodes_x + node.x];
        drawn = slot != kTerrainNoSlot;
    }
    float2 reach = float2(0.0);
    float3 low = float3(0.0);
    float3 high = float3(0.0);
    if (drawn)
    {
        const float3 camera = push.frame.camera_position.xyz;
        const float origin_y = float(int(push.frame.render_origin_steps.y)) * kMetersPerRenderStep;
        reach = push.patches[patch_index(info, patch)];
        const float2 corner = corner_of(patch, info.spacing_steps);
        low = float3(corner.x, reach.x - origin_y, corner.y);
        high = float3(corner.x + size_m, reach.y - origin_y, corner.y + size_m);

        // Its parent must have given way to it. That parent's children are this patch's node, which is here.
        if (push.level > 0)
        {
            const TerrainLevelInfo parent_info = push.levels[push.level - 1];
            const uint2 parent = patch / 2u;
            const float2 parent_reach = push.patches[patch_index(parent_info, parent)];
            const float2 parent_corner = corner_of(parent, parent_info.spacing_steps);
            const float parent_size = size_m * 2.0;
            const float parent_distance = box_distance(camera, float3(parent_corner.x, parent_reach.x - origin_y, parent_corner.y),
                                                       float3(parent_corner.x + parent_size, parent_reach.y - origin_y, parent_corner.y + parent_size));
            drawn = parent_distance < info.range_m;
        }
        // And it must not give way itself: its children have to be near enough, and there.
        if (drawn && push.level + 1 < push.level_count)
        {
            const TerrainLevelInfo child_info = push.levels[push.level + 1];
            const uint2 child_node = patch / 2u;
            if (child_node.x < child_info.nodes_x && child_node.y < child_info.nodes_z &&
                box_distance(camera, low, high) < child_info.range_m &&
                push.slots[child_info.first_node + child_node.y * child_info.nodes_x + child_node.x] != kTerrainNoSlot)
            {
                drawn = false;
            }
        }
    }
    if (drawn)
    {
        TerrainDraw draw;
        draw.level_x_z = terrain_pack(push.level, patch);
        draw.slot = slot;
        draw.base_steps = push.nodes[info.first_node + node.y * info.nodes_x + node.x].x;
        // Far enough to cover the gap to a neighbour that's still a level or two coarser, which grows with spacing and
        // with how rough the patch is, and no farther: it's drawn.
        draw.skirt_m = clamp((reach.y - reach.x) * 0.25, 2.0 * info.spacing_m, 16.0 * info.spacing_m);
        const float3 center = (low + high) * 0.5;
        const float3 half_size = (high - low) * 0.5 + float3(0.0, draw.skirt_m * 0.5, 0.0);
        for (uint v = 0; v < kCullViewCount; ++v)
        {
            if (box_in_view(push.views + v, center - float3(0.0, draw.skirt_m * 0.5, 0.0), half_size))
            {
                // Past the end, it takes back what it added: what's left counts exactly what was written.
                uint before;
                InterlockedAdd(push.args[v * kArgsWords + kArgsInstanceCount], 1, before);
                if (before < push.list_capacity)
                {
                    push.draws[v * push.list_capacity + before] = draw;
                }
                else
                {
                    InterlockedAdd(push.args[v * kArgsWords + kArgsInstanceCount], uint(-1), before);
                }
            }
        }
    }
}
