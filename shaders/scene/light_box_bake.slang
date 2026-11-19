// A light box's bake (docs/design.md, Global illumination: the plan from 2026-11-14, step 3), in compute, a thread a
// voxel: paths of light from the voxel's middle in directions spread evenly over the sphere (scene.path_trace, as the
// path tracer traces them), added to what its earlier rays brought, then the voxel's maps written from the sums so the
// picture sharpens as the bake goes on. Each frame adds a few rays to some of the voxels, until each has its share.
//
// What a voxel keeps is the light arriving at it from every way that isn't a light's direct light: what's bounced, and
// the sky's. As first-order spherical harmonics, put so that the light a surface facing n gathers, over pi (what
// scene.sky's hemisphere() gives: times a surface's albedo, the light it scatters), is a + b . n, with a the rays' mean
// and b twice their mean along their directions (the cosine lobe's first two bands, 1 and 2/3, times the harmonics'
// own factors, come to exactly that). A voxel whose rays mostly meet the backs of things is inside something: it
// keeps nothing, and the lookup leaves it out.

import common.bindless;
import scene.path_trace;
import scene.scene_common;

struct LightBoxBakePush
{
    SceneFrame* frame;
    float4* sums;        // per voxel, 4: [0] rgb the light's sum, a backs; [1..3] rgb the light times x, y and z, [1].a rays
    float4 origin_voxel; // xyz: the box's lowest corner, relative to the render origin; w: a voxel's size, in meters
    uint4 size;          // xyz: voxels each way; w: all of them
    uint first;          // the first voxel this dispatch adds to, counting x first, then y, then z
    uint count;          // and how many
    uint rays;           // each adds this many
    uint light_count;    // of the frame's local lights
    uint max_bounces;
    uint seed;
    uint maps[4];        // out, storage: [0] rgb a, alpha 1 where it isn't inside something; [1..3] rgb b's x, y, z
};

[[vk::push_constant]]
ConstantBuffer<LightBoxBakePush> push;

// A voxel with more than this share of its rays meeting backs is inside something.
static const float kInsideShare = 0.25;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    if (thread.x >= push.count)
    {
        return;
    }
    const uint voxel = (push.first + thread.x) % push.size.w;
    const uint3 size = push.size.xyz;
    const uint3 at = uint3(voxel % size.x, (voxel / size.x) % size.y, voxel / (size.x * size.y));
    SceneFrame* frame = push.frame;
    const float3 middle = push.origin_voxel.xyz + (float3(at) + 0.5) * push.origin_voxel.w;
    float4* sums = push.sums + voxel * 4;
    float4 light = sums[0];
    float4 along_x = sums[1];
    float4 along_y = sums[2];
    float4 along_z = sums[3];
    uint seed = path_hash(voxel * 0x9e3779b9u + 0x632be5abu) ^ path_hash(push.seed);
    for (uint r = 0; r < push.rays; ++r)
    {
        const float3 way = sphere_direction(next_unit(seed), next_unit(seed));
        const PathLight path = trace_path(frame, middle, way, seed, push.max_bounces, push.light_count, false);
        light += float4(path.light, path.back ? 1.0 : 0.0);
        along_x += float4(path.light * way.x, 1.0);
        along_y.rgb += path.light * way.y;
        along_z.rgb += path.light * way.z;
    }
    sums[0] = light;
    sums[1] = along_x;
    sums[2] = along_y;
    sums[3] = along_z;
    const float rays = max(along_x.a, 1.0);
    const bool inside = light.a > kInsideShare * rays;
    storage_3d(push.maps[0])[at] = float4(min(light.rgb / rays, kHalfMax), inside ? 0.0 : 1.0);
    storage_3d(push.maps[1])[at] = float4(clamp(2.0 * along_x.rgb / rays, -kHalfMax, kHalfMax), 0.0);
    storage_3d(push.maps[2])[at] = float4(clamp(2.0 * along_y.rgb / rays, -kHalfMax, kHalfMax), 0.0);
    storage_3d(push.maps[3])[at] = float4(clamp(2.0 * along_z.rgb / rays, -kHalfMax, kHalfMax), 0.0);
}
