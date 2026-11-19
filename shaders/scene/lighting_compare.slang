// The rasterized light against the path tracer's (docs/design.md, Global illumination: the plan from 2026-11-14, step
// 1), in compute, a thread a pixel: how far the one is from the other in stops, as a heat map (grey where they agree,
// blue where the rasterized picture is too dark, red where it's too bright, full at a stop off) and summed for the
// stats. Both are the light leaving each surface toward the camera, before the fog. The sky isn't counted.

import common.bindless;
import scene.scene_common;

struct LightingComparePush
{
    uint* counts;  // pixels; the sum of |stops| in 1024ths; over 10% off; over 25%; too dark by 10%; too bright by 10%
    uint raster;   // the rasterized light (sampled)
    uint traced;   // the path tracer's mean (sampled): rgb
    uint depth;    // the scene's depth (sampled): 0 is the sky
    uint heat;     // out: the heat map (storage)
    uint2 size;
};

[[vk::push_constant]]
ConstantBuffer<LightingComparePush> push;

// Off by more than these, in stops: 10% and 25%.
static const float kTenPercent = 0.137503524;
static const float kQuarter = 0.321928095;
// The most a pixel counts for in the mean, in stops.
static const float kMostCounted = 2.0;
// Light below this counts as this: two dark pixels agree, whatever their ratio.
static const float kFloor = 1e-4;

groupshared uint sums[6];

float luminance(float3 c)
{
    return dot(max(c, 0.0), float3(0.2126, 0.7152, 0.0722));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID, uint index: SV_GroupIndex)
{
    if (index < 6)
    {
        sums[index] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    const uint2 texel = thread.xy;
    if (all(texel < push.size))
    {
        RWTexture2D<float4> heat = storage_2d(push.heat);
        const float depth = load_texture(push.depth, int2(texel)).r;
        const float truth = luminance(load_texture(push.traced, int2(texel)).rgb);
        // A dim picture of the scene underneath, so it reads as a place.
        const float shade = 0.05 + 0.25 * saturate(log2(1.0 + truth * 8.0) / 4.0);
        if (depth <= 0.0)
        {
            heat[texel] = float4(float3(0.02), 1.0); // the sky: not compared
        }
        else
        {
            const float drawn = luminance(load_texture(push.raster, int2(texel)).rgb);
            const float stops = log2(max(drawn, kFloor) / max(truth, kFloor));
            const float t = clamp(stops, -1.0, 1.0);
            const float3 dark = float3(0.10, 0.35, 1.00);
            const float3 bright = float3(1.00, 0.25, 0.10);
            heat[texel] = float4(lerp(float3(shade), t < 0.0 ? dark : bright, abs(t)), 1.0);
            const float off = abs(stops);
            InterlockedAdd(sums[0], 1);
            // In the mean, a pixel counts as 2 stops off at most: one black in one picture and dimly lit in the other
            // (a shadow's edge) is ten stops off, and a few of them would drown the rest.
            InterlockedAdd(sums[1], uint(min(off, kMostCounted) * 1024.0 + 0.5));
            InterlockedAdd(sums[2], off > kTenPercent ? 1u : 0u);
            InterlockedAdd(sums[3], off > kQuarter ? 1u : 0u);
            InterlockedAdd(sums[4], stops < -kTenPercent ? 1u : 0u);
            InterlockedAdd(sums[5], stops > kTenPercent ? 1u : 0u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (index < 6 && sums[index] != 0)
    {
        InterlockedAdd(push.counts[index], sums[index]);
    }
}
