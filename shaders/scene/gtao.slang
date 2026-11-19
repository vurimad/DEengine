// Ambient occlusion in screen space (docs/design.md, Global illumination: GTAO): how much of the hemisphere above
// each pixel's surface is open, weighted by the cosine as diffuse light is, for the small and near things the
// probes are too coarse to see (a step's foot, the gap under a crate, a fold of cloth).
//
// The method is Jimenez et al., "Practical Realtime Strategies for Accurate Indirect Occlusion" (SIGGRAPH 2016,
// "ground truth ambient occlusion"), written from the paper. Per pixel, a few slices: planes through the view
// direction, each along a direction on the screen. Along each, both ways, the highest horizon the depth buffer
// shows within a radius in the world; the visible arc between the two horizons, with the normal projected into the
// slice, integrates in closed form to the cosine-weighted visibility of that slice. Over every direction of slice
// that averages to the visibility; over a pixel's few, an open surface seen at a slant comes to more than 1 in some
// and less in others. So the pixel's is the sum over its slices against what the same slices give with nothing in
// the way: an open surface is exactly open, at any slant, with no noise, and what occludes keeps its share.
//
// - Samples spread out quadratically from the pixel (more near, where occlusion is sharp), and farther ones are
//   read from coarser mips of scene.ao_depth's pyramid.
// - A sample fades out of the horizon toward the radius, so far things don't occlude and a step's edge fades
//   rather than cuts.
// - Each pixel's slices turn by interleaved gradient noise, and the samples along them start a noise-sized step
//   in: neighbours together cover the directions, and scene.ao_denoise spreads it. With TAA on the noise moves
//   each frame and TAA averages it as well.
// - Everything is in the camera's space turned to the world's (positions relative to the camera, the world's axes),
//   so the G-buffer's world normals are used as they are.

import common.bindless;
import scene.scene_common;

static const float kHalfPi = 1.57079633;
static const float kAoPi = 3.14159265;
static const float kAoFarM = 1000000.0;

struct GtaoPush
{
    SceneFrame* frame;
    uint64_t reserved;
    float4 right;   // xyz: the camera's right, unit; w: the tangent of half its horizontal field of view
    float4 up;      // xyz: its up; w: of half its vertical one
    float4 forward; // xyz: where it looks
    uint depth;     // scene.ao_depth's pyramid (sampled, every mip)
    uint normals;   // the G-buffer's normals (gb1)
    uint output;    // this pass's, as a storage texture
    uint slices;
    uint steps;           // along each side of a slice
    float radius_m;       // what occludes is within this of the surface
    float falloff_share;  // of the radius, where a sample starts to fade out of the horizon
    float max_radius_px;  // the radius on the screen, at most
    uint2 size;
    float2 texel; // 1 over the size
};

[[vk::push_constant]]
ConstantBuffer<GtaoPush> push;

// The point a pixel's depth stands for, relative to the camera, in the world's axes. uv: across the picture, from
// its top left; view_m: depth along the view. The picture was drawn shifted by TAA's offset: taken out again.
float3 camera_point(float2 uv, float view_m, float2 offset_ndc)
{
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - offset_ndc;
    return (push.forward.xyz + push.right.xyz * (ndc.x * push.right.w) + push.up.xyz * (ndc.y * push.up.w)) * view_m;
}

// The visible part of a slice's arc between the normal's angle n and a horizon's angle h, by the cosine: the
// paper's closed form.
float arc(float h, float n, float cos_n, float sin_n)
{
    return 0.25 * (cos_n + 2.0 * h * sin_n - cos(2.0 * h - n));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const uint2 texel = thread.xy;
    if (any(texel >= push.size))
    {
        return;
    }
    RWTexture2D<float4> output = storage_2d(push.output);
    SceneFrame* frame = push.frame;
    const float view_m = load_texture(push.depth, int2(texel)).r;
    if (view_m >= kAoFarM * 0.5)
    {
        output[texel] = float4(1.0, 0.0, 0.0, 0.0); // the sky
        return;
    }
    const float4 taa = frame.taa_params;
    const float2 offset_ndc = taa.zw;
    const float2 uv = (float2(texel) + 0.5) * push.texel;
    const float3 p = camera_point(uv, view_m, offset_ndc);
    const float3 v = normalize(-p);
    const float3 n_world = gbuffer_normal(load_texture(push.normals, int2(texel)).xy);
    // The radius on the screen: what it spans at this depth, in pixels. Too small to find anything: nothing.
    const float radius_px = min(push.radius_m / (view_m * push.up.w) * (float(push.size.y) * 0.5), push.max_radius_px);
    if (radius_px < 1.0)
    {
        output[texel] = float4(1.0, 0.0, 0.0, 0.0);
        return;
    }
    // The noise: one value turns the slices, another starts the steps; with TAA, each frame its own.
    const float2 moved = float2(texel) + float2(5.588238, 5.588238) * taa.y;
    const float turn = gradient_noise(moved);
    const float start = gradient_noise(moved.yx + float2(23.0, 61.0));
    const float falloff_from = push.radius_m * push.falloff_share;
    const float falloff_over = max(push.radius_m - falloff_from, 1e-4);
    float visibility = 0.0;
    float open = 0.0; // what the same slices give with nothing in the way
    for (uint slice = 0; slice < push.slices; ++slice)
    {
        const float angle = (float(slice) + turn) / float(push.slices) * kAoPi;
        const float2 direction = float2(cos(angle), sin(angle)); // on the screen, y down
        // The slice's plane in the world: the view direction and the way the screen's direction goes.
        const float3 along = push.right.xyz * direction.x - push.up.xyz * direction.y;
        const float3 ortho = normalize(along - v * dot(along, v));
        const float3 axis = cross(ortho, v);
        const float3 projected = n_world - axis * dot(n_world, axis);
        const float projected_length = length(projected);
        if (projected_length < 1e-4)
        {
            continue; // the normal lies across the slice: it sees nothing of it
        }
        const float cos_n = saturate(dot(projected, v) / projected_length);
        const float n = (dot(projected, ortho) >= 0.0 ? 1.0 : -1.0) * acos(cos_n);
        const float sin_n = sin(n);
        // The lowest each horizon can be: the surface's own plane.
        const float low_plus = cos(n + kHalfPi);
        const float low_minus = cos(n - kHalfPi);
        float horizon_plus = low_plus;
        float horizon_minus = low_minus;
        for (uint step = 0; step < push.steps; ++step)
        {
            float t = (float(step) + start) / float(push.steps);
            t *= t;
            const float offset_px = t * radius_px + 1.0;
            // Farther samples from coarser mips, read at their texel's middle: a mip's depth stands for that point,
            // and a position rebuilt anywhere else would put a slanted plane above or below itself.
            const uint mip = uint(clamp(log2(offset_px) - 3.3, 0.0, 4.0));
            const float2 mip_size = float2(max(push.size >> mip, uint2(1u)));
            const float2 offset_uv = direction * (offset_px * push.texel);
            for (uint side = 0; side < 2; ++side)
            {
                const float2 wanted_uv = side == 0 ? uv + offset_uv : uv - offset_uv;
                if (any(wanted_uv < 0.0) || any(wanted_uv > 1.0))
                {
                    continue; // off the picture: nothing known there
                }
                const int2 sample_texel = int2(min(wanted_uv * mip_size, mip_size - 1.0));
                const float2 sample_uv = (float2(sample_texel) + 0.5) / mip_size;
                const float sample_m = load_texture(push.depth, sample_texel, int(mip)).r;
                const float3 delta = camera_point(sample_uv, sample_m, offset_ndc) - p;
                const float distance = length(delta);
                const float cos_h = dot(delta, v) / max(distance, 1e-5);
                const float counts = saturate((push.radius_m - distance) / falloff_over);
                if (side == 0)
                {
                    horizon_plus = max(horizon_plus, lerp(low_plus, cos_h, counts));
                }
                else
                {
                    horizon_minus = max(horizon_minus, lerp(low_minus, cos_h, counts));
                }
            }
        }
        // The horizons as angles from the view direction, the + side's positive, kept within the surface's side.
        const float h_plus = n + min(acos(clamp(horizon_plus, -1.0, 1.0)) - n, kHalfPi);
        const float h_minus = n + max(-acos(clamp(horizon_minus, -1.0, 1.0)) - n, -kHalfPi);
        visibility += projected_length * (arc(h_plus, n, cos_n, sin_n) + arc(h_minus, n, cos_n, sin_n));
        open += projected_length * (cos_n + n * sin_n); // the two arcs with the horizons at the surface's plane
    }
    output[texel] = float4(open > 1e-4 ? saturate(visibility / open) : 1.0, 0.0, 0.0, 0.0);
}
