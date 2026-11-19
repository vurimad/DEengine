// Which local lights reach each cluster of the view (docs/design.md, Local lights), in compute: a thread a cluster,
// each testing every light of the frame against its cluster's box and writing a bit for those that reach it. The
// lights were culled to the view on the CPU; what's left is few (render::kMaxFrameLights at most), so every cluster
// against every light is cheaper than anything cleverer, and has no list that can overflow.
//
// A cluster is the part of the view's pyramid behind one cell of a grid across the picture, between two depths; its
// box here is the smallest one, in the camera's axes, that holds it. The last slice has no far side.

import scene.scene_common;

struct LightCullPush
{
    SceneFrame* frame; // its lights and grid are read; its clusters' words written
    uint light_count;
    uint cluster_count;
    float4 camera_right;       // xyz: the camera's right, w: the tangent of half the view's width
    float4 camera_up;          // xyz: its up, w: the tangent of half its height
    float slice_near_m;        // where the first slice ends
    float slices_per_doubling; // of depth
    uint2 reserved;
};

[[vk::push_constant]]
ConstantBuffer<LightCullPush> push;

static const float kNoFarSideM = 1e7;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const uint cluster = thread.x;
    if (cluster >= push.cluster_count)
    {
        return;
    }
    SceneFrame* frame = push.frame;
    const uint3 grid = frame.light_grid.xyz;
    const uint words = frame.light_grid.w;
    const uint x = cluster % grid.x;
    const uint y = (cluster / grid.x) % grid.y;
    const uint z = cluster / (grid.x * grid.y);

    // The cluster's box in the camera's axes: right, up, and depth along the view.
    const float2 low = float2(x, y) / float2(grid.xy) * 2.0 - 1.0;
    const float2 high = float2(x + 1, y + 1) / float2(grid.xy) * 2.0 - 1.0;
    const float near = z == 0 ? 0.0 : push.slice_near_m * exp2(float(z) / push.slices_per_doubling);
    const float far = z + 1 == grid.z ? kNoFarSideM : push.slice_near_m * exp2(float(z + 1) / push.slices_per_doubling);
    const float2 tangent = float2(push.camera_right.w, push.camera_up.w);
    // A side that leans outward is farthest out at the far depth, one that leans inward at the near.
    const float3 box_min = float3(min(low * near, low * far) * tangent, near);
    const float3 box_max = float3(max(high * near, high * far) * tangent, far);

    const float3 camera = frame.camera_position.xyz;
    for (uint word = 0; word < words; ++word)
    {
        uint mask = 0;
        const uint first = word * 32;
        const uint count = min(push.light_count - min(first, push.light_count), 32u);
        for (uint bit = 0; bit < count; ++bit)
        {
            const float4 sphere = frame.lights[first + bit].position_radius;
            const float3 offset = sphere.xyz - camera;
            const float3 center = float3(dot(offset, push.camera_right.xyz), dot(offset, push.camera_up.xyz),
                                         dot(offset, frame.camera_forward.xyz));
            // How far the light's middle is from the box: within its radius, it reaches the cluster.
            const float3 away = max(max(box_min - center, center - box_max), float3(0.0));
            if (dot(away, away) <= sphere.w * sphere.w)
            {
                mask |= 1u << bit;
            }
        }
        frame.light_clusters[cluster * words + word] = mask;
    }
}
