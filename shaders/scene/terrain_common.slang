// What terrain's passes share (docs/design.md, Terrain & vegetation): how a patch's vertices are found from the heights.
// Matches engine/render/scene/terrain.cpp and asset/terrain_format.h.

import scene.scene_common;

public static const uint kTerrainPatchQuads = 32;
public static const uint kTerrainGridSide = kTerrainPatchQuads + 1;
public static const uint kTerrainGridVertices = kTerrainGridSide * kTerrainGridSide;
public static const uint kTerrainBlockBorder = 2;
public static const uint kTerrainBlockSamples = 133;
public static const uint kTerrainBlockWords = (kTerrainBlockSamples * kTerrainBlockSamples * 2 + 3) / 4;
public static const uint kTerrainNoSlot = 0xffffffffu;
public static const uint kTerrainMaxLevels = 8;

// One level of the pyramid.
public struct TerrainLevelInfo
{
    public uint nodes_x;
    public uint nodes_z;
    public uint first_node;
    public uint first_patch;
    public uint spacing_steps;  // between samples, in kMetersPerRenderStep
    public float spacing_m;
    public float step_m;        // what one in a sample is worth
    public float range_m;       // a patch of this level draws only this near. Infinite at the root
    public float morph_start_m; // vertices begin turning into the coarser level's shape here
    public float morph_scale;   // and over this many per meter they finish. 0: the root has nothing to turn into
    public uint2 padding;
};

// A patch to draw, in the list each view has.
public struct TerrainDraw
{
    public uint level_x_z;  // level << 28, x << 14, z: which patch of that level
    public uint slot;       // where its node's heights are
    public int base_steps;  // what its node's samples add to
    public float skirt_m;   // how far its edges hang
};

public uint terrain_pack(uint level, uint2 patch)
{
    return (level << 28) | (patch.x << 14) | patch.y;
}

// Where a vertex of a patch ends up.
public struct TerrainPoint
{
    public float3 position; // relative to the render origin
    public float3 normal;
    public float height_m;  // in the world
};

float block_sample(uint* heights, uint block_word, int2 at)
{
    const uint index = uint(at.y) * kTerrainBlockSamples + uint(at.x);
    const uint word = heights[block_word + index / 2];
    return float((word >> ((index & 1u) * 16u)) & 0xffffu);
}

// The vertices are a grid of the patch's squares, then a row hanging from each of its four sides.
uint2 grid_vertex(uint vertex_id, out bool skirt)
{
    skirt = vertex_id >= kTerrainGridVertices;
    if (!skirt)
    {
        return uint2(vertex_id % kTerrainGridSide, vertex_id / kTerrainGridSide);
    }
    const uint hanging = vertex_id - kTerrainGridVertices;
    const uint along = hanging % kTerrainGridSide;
    switch (hanging / kTerrainGridSide)
    {
    case 0: return uint2(along, 0u);
    case 1: return uint2(along, kTerrainPatchQuads);
    case 2: return uint2(0u, along);
    default: return uint2(kTerrainPatchQuads, along);
    }
}

// normal: whether to work out the normal, which most of the reads are for.
public TerrainPoint terrain_point(uint vertex_id, TerrainDraw draw, TerrainLevelInfo* levels, uint* heights,
                                  SceneFrame* frame, uint2 origin_steps, uint2 size_steps, bool normal)
{
    bool skirt;
    const uint2 vertex = grid_vertex(vertex_id, skirt);
    const uint level = draw.level_x_z >> 28;
    const uint2 patch = uint2((draw.level_x_z >> 14) & 0x3fffu, draw.level_x_z & 0x3fffu);
    const TerrainLevelInfo info = levels[level];
    const uint block_word = draw.slot * kTerrainBlockWords;

    // In the node, and so in its block. The node's corner is on the coarser level's grid: that level's vertices are the
    // even ones here.
    const int2 in_node = int2((patch % 4u) * kTerrainPatchQuads + vertex);
    const int2 at = in_node + int2(int(kTerrainBlockBorder));
    const float own = block_sample(heights, block_word, at);

    // Where it is across: stopping at the edge of the map, which a node's far side can pass. In whole steps from the
    // map's corner, then off the render origin, so it holds anywhere in the world.
    const uint2 map_steps = min((patch * kTerrainPatchQuads + vertex) * info.spacing_steps, size_steps);
    const uint3 origin = frame.render_origin_steps.xyz;
    const float2 across = float2(int2(origin_steps + map_steps - origin.xz)) * kMetersPerRenderStep;
    const float origin_y = float(int(origin.y)) * kMetersPerRenderStep;
    const float own_m = (float(draw.base_steps) + own) * info.step_m;

    // How far it has turned into the coarser level: which makes it what that level's surface is here, so the two meet
    // along an edge, and nothing moves when one takes over. Only height moves. A coarse square is two triangles along
    // the same diagonal as the fine ones, so its middle is halfway along that diagonal.
    const float3 before = float3(across.x, own_m - origin_y, across.y);
    const float morph = saturate((length(before - frame.camera_position.xyz) - info.morph_start_m) * info.morph_scale);
    float value = own;
    if (morph > 0.0)
    {
        const int2 odd = in_node & int2(1);
        const float coarse = (block_sample(heights, block_word, at - odd) + block_sample(heights, block_word, at + odd)) * 0.5;
        value = lerp(own, coarse, morph);
    }

    TerrainPoint point;
    point.height_m = (float(draw.base_steps) + value) * info.step_m;
    point.position = float3(across.x, point.height_m - origin_y, across.y);
    point.normal = float3(0.0, 1.0, 0.0);
    if (skirt)
    {
        point.position.y -= draw.skirt_m;
    }
    if (normal)
    {
        const float per_sample = info.step_m / (2.0 * info.spacing_m);
        const int2 dx = int2(1, 0);
        const int2 dz = int2(0, 1);
        float2 slope = float2(block_sample(heights, block_word, at + dx) - block_sample(heights, block_word, at - dx),
                              block_sample(heights, block_word, at + dz) - block_sample(heights, block_word, at - dz)) * per_sample;
        if (morph > 0.0)
        {
            // The coarser level's slope: its samples are two apart. The border is there for this.
            const float2 coarse = float2(block_sample(heights, block_word, at + 2 * dx) - block_sample(heights, block_word, at - 2 * dx),
                                         block_sample(heights, block_word, at + 2 * dz) - block_sample(heights, block_word, at - 2 * dz)) * per_sample * 0.5;
            slope = lerp(slope, coarse, morph);
        }
        point.normal = normalize(float3(-slope.x, 1.0, -slope.y));
    }
    return point;
}
