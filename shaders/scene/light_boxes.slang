// Reading the light boxes (docs/design.md, Global illumination: the plan from 2026-11-14, step 3): the light a
// surface gathers from what's bounced and from the sky, baked into boxes of voxels (scene.light_box_bake). A point
// takes the finest box that holds it, faded out over the box's edge; within it, the eight voxels round a point half a
// voxel off the surface, by nearness and by how far each is in front of the surface, leaving out those inside
// something (whose light is nothing anyone sees). Where no box holds a point, the flat ambient (scene.sky's
// hemisphere()) is what's there.

import common.bindless;
import scene.scene_common;

// What the light boxes give a surface.
public struct BoxLight
{
    public float3 diffuse; // the light gathered round its normal, over pi, as hemisphere() gives it
    public float3 mirror;  // the same round its mirror direction: a rough reflection's
    public float weight;   // how much of that is the boxes' to say: 0 where none holds the point
};

// The light a surface facing n gathers from a voxel's maps, over pi: a + b . n, never below nothing.
float3 voxel_light(GpuLightBox box, int3 at, float3 n, out float valid)
{
    const float4 a = texture_3d(box.maps.x).Load(int4(at, 0));
    valid = a.a;
    const float3 bx = texture_3d(box.maps.y).Load(int4(at, 0)).rgb;
    const float3 by = texture_3d(box.maps.z).Load(int4(at, 0)).rgb;
    const float3 bz = texture_3d(box.maps.w).Load(int4(at, 0)).rgb;
    return max(a.rgb + bx * n.x + by * n.y + bz * n.z, 0.0);
}

// One box's light at a point, for a surface facing n and for its mirror direction.
BoxLight box_light(GpuLightBox box, float3 position, float3 normal, float3 mirror, bool mirrors)
{
    BoxLight light;
    light.diffuse = float3(0.0);
    light.mirror = float3(0.0);
    light.weight = 0.0;
    const float voxel = box.origin_voxel.w;
    const float3 grid = (position + normal * (0.5 * voxel) - box.origin_voxel.xyz) / voxel - 0.5;
    const int3 last = int3(box.size.xyz) - 1;
    const float3 clamped = clamp(grid, float3(0.0), float3(last));
    const int3 first = min(int3(floor(clamped)), max(last - 1, int3(0)));
    const float3 part = saturate(clamped - float3(first));
    float total = 0.0;
    for (uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1u, (corner >> 1) & 1u, (corner >> 2) & 1u);
        const int3 at = min(first + offset, last);
        const float3 share = lerp(1.0 - part, part, float3(offset));
        float valid;
        const float3 diffuse = voxel_light(box, at, normal, valid);
        // A voxel behind the surface sees its other side, and one beside it half of the surface itself: by how much
        // it's in front (the DDGI papers' weight for probes), so a face turned against the grid doesn't take a
        // staircase of voxels cut by it.
        const float3 toward = box.origin_voxel.xyz + (float3(at) + 0.5) * voxel - position;
        const float facing = (dot(toward * rsqrt(max(dot(toward, toward), 1e-8)), normal) + 1.0) * 0.5;
        const float weight = share.x * share.y * share.z * valid * (facing * facing + 0.02);
        if (weight <= 0.0)
        {
            continue;
        }
        light.diffuse += diffuse * weight;
        if (mirrors)
        {
            float unused;
            light.mirror += voxel_light(box, at, mirror, unused) * weight;
        }
        total += weight;
    }
    if (total > 1e-5)
    {
        light.diffuse /= total;
        light.mirror /= total;
        light.weight = 1.0;
    }
    return light;
}

// The light boxes' light at a point: the finest box holding it, its weight fading to 0 over the box's outermost fade
// distance (box.params.x). Weight 0 where no box holds it, or where every voxel round it is inside something.
public BoxLight light_box_light(SceneFrame* frame, float3 position, float3 normal, float3 mirror, bool mirrors = true)
{
    BoxLight best;
    best.diffuse = float3(0.0);
    best.mirror = float3(0.0);
    best.weight = 0.0;
    float best_voxel = 1e30;
    float best_edge = 0.0;
    for (uint b = 0; b < frame.light_box_count; ++b)
    {
        const GpuLightBox box = frame.light_boxes[b];
        const float3 low = box.origin_voxel.xyz;
        const float3 high = low + float3(box.size.xyz) * box.origin_voxel.w;
        const float3 inside = min(position - low, high - position);
        const float depth = min(inside.x, min(inside.y, inside.z));
        if (depth <= 0.0 || box.origin_voxel.w >= best_voxel)
        {
            continue;
        }
        best_voxel = box.origin_voxel.w;
        best_edge = saturate(depth / max(box.params.x, 1e-4));
        best = box_light(box, position, normal, mirror, mirrors);
    }
    best.weight *= best_edge;
    return best;
}
