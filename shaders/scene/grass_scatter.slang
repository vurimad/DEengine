// Scatters grass around the camera (docs/design.md, Terrain & vegetation: grass fields). One thread for each square of the
// finest terrain level near the camera: it reads the square's four heights once, and places its blades on them. A blade's
// place comes from a hash of its square and its number, so it stays where it grew however the camera moves, and as the
// camera moves off, the last-numbered blades go first.

import common.bindless;
import scene.grass_common;
import scene.scene_common;
import scene.terrain_common;

// Matches GrassScatterPush in engine/render/scene/terrain.cpp.
struct GrassScatterPush
{
    SceneFrame* frame;
    GpuCullView* views;
    TerrainLevelInfo* levels;
    int2* nodes;      // per node: asset::TerrainNode
    uint* slots;      // per node: where its heights are, or kTerrainNoSlot
    uint* heights;
    GrassBlade* blades;
    uint* args;       // per level of detail: a DrawIndexedIndirect's five words
    Grass* grass;
    GpuMaterial* materials;
    uint2 origin_steps;
    uint2 size_steps;
    uint level;       // the finest
    uint masks_material;
    uint has_masks;
};

[[vk::push_constant]]
ConstantBuffer<GrassScatterPush> push;

static const uint kCellsSide = 224; // 2 m squares: covers kGrassReachM to each side

uint mix_bits(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float unit_from(uint bits)
{
    return float(bits & 0xffffu) / 65535.0;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    if (thread.x >= kCellsSide || thread.y >= kCellsSide)
    {
        return;
    }
    const TerrainLevelInfo info = push.levels[push.level];
    const uint3 origin = push.frame.render_origin_steps.xyz;
    const float3 camera = push.frame.camera_position.xyz;
    const float2 origin_on_map = float2(int2(origin.xz - push.origin_steps)) * kMetersPerRenderStep;
    const int2 camera_cell = int2(floor((camera.xz + origin_on_map) / info.spacing_m));
    const int2 signed_cell = camera_cell + int2(thread.xy) - int2(kCellsSide / 2u);
    const uint2 size_cells = push.size_steps / info.spacing_steps;
    if (signed_cell.x < 0 || signed_cell.y < 0 || signed_cell.x >= int(size_cells.x) || signed_cell.y >= int(size_cells.y))
    {
        return;
    }
    const uint2 cell = uint2(signed_cell);
    const float2 corner = float2(int2(push.origin_steps + cell * info.spacing_steps - origin.xz)) * kMetersPerRenderStep;
    const float2 middle = corner + info.spacing_m * 0.5;
    const float distance_m = length(middle - camera.xz);
    if (distance_m >= kGrassReachM)
    {
        return;
    }
    // Behind the camera: most of these squares, known before reading anything. Looking down, the view spreads all round.
    const float2 forward = push.frame.camera_forward.xz;
    const float forward_flat = length(forward);
    if (distance_m > 8.0 && forward_flat > 1e-3 && dot((middle - camera.xz) / distance_m, forward / forward_flat) < 0.35 - (1.0 - forward_flat) * 2.5)
    {
        return;
    }

    // Its heights have to be here: until they are, there's nothing to stand on.
    const uint2 node = cell / kTerrainNodeQuads;
    const uint node_index = info.first_node + node.y * info.nodes_x + node.x;
    const uint slot = push.slots[node_index];
    if (slot == kTerrainNoSlot)
    {
        return;
    }
    const int2 at = int2(cell % kTerrainNodeQuads) + int2(int(kTerrainBlockBorder));
    const uint block_word = slot * kTerrainBlockWords;
    const float base = float(push.nodes[node_index].x);
    const float h00 = (base + block_sample(push.heights, block_word, at)) * info.step_m;
    const float h10 = (base + block_sample(push.heights, block_word, at + int2(1, 0))) * info.step_m;
    const float h01 = (base + block_sample(push.heights, block_word, at + int2(0, 1))) * info.step_m;
    const float h11 = (base + block_sample(push.heights, block_word, at + int2(1, 1))) * info.step_m;
    const float height_m = (h00 + h10 + h01 + h11) * 0.25;

    // In the view? A square and the tallest blade.
    const float origin_y = float(int(origin.y)) * kMetersPerRenderStep;
    const float3 center = float3(middle.x, height_m - origin_y + 0.4, middle.y);
    for (uint p = 0; p < push.views[0].plane_count; ++p)
    {
        const float4 plane = push.views[0].planes[p];
        if (dot(plane.xyz, center) + plane.w < -3.0)
        {
            return;
        }
    }

    // Does it grow here?
    const float2 slope = float2(h10 - h00 + h11 - h01, h01 - h00 + h11 - h10) * 0.5 / info.spacing_m;
    const float up = 1.0 / sqrt(1.0 + dot(slope, slope));
    const float2 world_xz = middle + float2(int2(origin.xz)) * kMetersPerRenderStep;
    float4 masks = float4(0.0);
    if (push.has_masks != 0)
    {
        const float2 size_m = float2(push.size_steps) * kMetersPerRenderStep;
        const float2 corner_m = float2(push.origin_steps) * kMetersPerRenderStep;
        const uint masks_texture = push.materials[push.masks_material].metallic_roughness_texture;
        masks = texture_2d(masks_texture).SampleLevel(sampler_state(kSamplerLinearClamp), (world_xz - corner_m) / size_m, 0.0);
    }
    float weight = grass_where(push.grass, height_m, up, masks);
    if (weight <= 0.0)
    {
        return;
    }
    weight *= 1.0 - smoothstep(kGrassReachM * kGrassFadeFrom, kGrassReachM, distance_m); // thins away, rather than stopping at a line
    const float tufts = grass_noise(world_xz / 5.5) * 0.6 + grass_noise(world_xz / 23.0) * 0.4;
    weight *= saturate(1.0 - push.grass.patches * 1.8 * max(0.5 - tufts, 0.0) * 2.0);
    if (weight <= 0.0)
    {
        return;
    }

    const uint lod = distance_m < kGrassLodEndM[0] ? 0u : distance_m < kGrassLodEndM[1] ? 1u : 2u;
    const float wanted = push.grass.density_per_m2 * info.spacing_m * info.spacing_m * weight * grass_kept(distance_m);
    uint count = uint(ceil(wanted));
    if (count == 0u)
    {
        return;
    }
    // Room for them; past the end, it gives back what doesn't fit.
    uint before;
    InterlockedAdd(push.args[lod * kGrassArgsWords + kGrassArgsInstanceCount], count, before);
    const uint capacity = kGrassLodCapacity[lod];
    if (before + count > capacity)
    {
        const uint fits = before < capacity ? capacity - before : 0u;
        uint ignored;
        InterlockedAdd(push.args[lod * kGrassArgsWords + kGrassArgsInstanceCount], fits - count, ignored);
        count = fits;
    }
    const uint other_mask = push.grass.other_mask;
    const uint other = other_mask < 4u ? uint(smoothstep(0.3, 0.7, masks[other_mask]) * 15.0 + 0.5) : 0u;
    const uint seed = mix_bits(cell.x * 73856093u ^ cell.y * 19349663u);
    const float clump = grass_noise(world_xz / 3.1);
    const float slow_tone = grass_slow_tone(world_xz);
    for (uint i = 0; i < count; ++i)
    {
        const uint a = mix_bits(seed + i);
        const uint b = mix_bits(a);
        const float2 inside = float2(unit_from(a), unit_from(a >> 16));
        const float along_x = lerp(h00, h10, inside.x);
        const float along_x2 = lerp(h01, h11, inside.x);
        GrassBlade blade;
        blade.root = float3(corner.x + inside.x * info.spacing_m, lerp(along_x, along_x2, inside.y) - origin_y - 0.03, corner.y + inside.y * info.spacing_m);
        // The last one in shrinks in as it comes, so none pops.
        const float last = saturate(wanted - float(i));
        const float height_share = saturate((0.25 + 0.75 * unit_from(b)) * (0.55 + 0.45 * clump)) * last * sqrt(weight);
        const uint yaw = (b >> 16) & 0xffu;
        const uint height = uint(height_share * 255.0);
        const uint tone = uint(saturate(0.7 * slow_tone + 0.3 * unit_from(mix_bits(b))) * 255.0);
        const uint lean = (mix_bits(b) >> 16) & 0xfu;
        blade.packed = yaw | (height << 8u) | (tone << 16u) | (lean << 24u) | (other << 28u);
        push.blades[kGrassLodFirst[lod] + before + i] = blade;
    }
}
