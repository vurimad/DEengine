// The path tracer (docs/design.md, Global illumination: the plan from 2026-11-14, step 1): the light of a view by
// following paths of light (scene.path_trace), a sample a pixel a frame added to the mean of those before, until it
// converges. A renderer of its own beside the rasterized one, and what every fast layer of the lighting is measured
// against: same rays, materials, lights, sky and conventions as the rasterized picture, so where the two differ the
// fast one is wrong.

import common.bindless;
import scene.path_trace;
import scene.scene_common;

struct PathTracerPush
{
    SceneFrame* frame;
    uint accumulation; // the mean so far (RGBA32F storage): rgb the mean, a how many samples it holds
    uint sample;       // samples it holds now; 0: it starts again
    uint2 size;
    uint light_count;  // of the frame's local lights
    uint max_bounces;
};

[[vk::push_constant]]
ConstantBuffer<PathTracerPush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const uint2 texel = thread.xy;
    if (any(texel >= push.size))
    {
        return;
    }
    SceneFrame* frame = push.frame;
    uint seed = path_hash(texel.x + texel.y * 65536u) ^ path_hash(push.sample * 0x9e3779b9u + 0x85ebca6bu);
    // Through a point of the pixel: a new one each sample, so edges come out as the pixel's mean.
    const float2 at = (float2(texel) + float2(next_unit(seed), next_unit(seed))) / float2(push.size);
    const float2 ndc = float2(at.x * 2.0 - 1.0, 1.0 - at.y * 2.0);
    const float3 origin = frame.camera_position.xyz;
    const float3 direction = normalize(position_from_depth(frame, ndc, 1.0) - origin);
    const float3 value = trace_path(frame, origin, direction, seed, push.max_bounces, push.light_count, true).light;
    RWTexture2D<float4> accumulation = storage_2d(push.accumulation);
    if (push.sample == 0)
    {
        accumulation[texel] = float4(value, 1.0);
    }
    else
    {
        const float4 before = accumulation[texel];
        accumulation[texel] = float4(before.rgb + (value - before.rgb) / float(push.sample + 1), float(push.sample + 1));
    }
}
