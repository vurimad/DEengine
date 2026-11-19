// The ambient occlusion's depth pyramid (docs/design.md, Global illumination: GTAO), in compute: mip 0 is each
// pixel's depth along the view in meters, from the scene's reverse-Z depth; each mip after it is a quarter of the
// one before, the four texels below a texel averaged (on a smooth surface, the depth at the texel's middle), but
// over an edge only those near the nearest: a far one behind it would make a surface between the two that isn't
// there. Near means within the spread a smooth surface has across the four at that depth, seen at a slant. scene.gtao reads farther samples from coarser mips: fewer reads, and
// the cache keeps them.

import common.bindless;
import scene.scene_common;

// What the sky's depth becomes: farther than anything that occludes.
static const float kAoFarM = 1000000.0;

struct AoDepthPush
{
    uint depth;        // mip 0: the scene's depth (sampled); otherwise the mip before, as a storage texture
    uint output;       // this mip, as a storage texture
    uint2 size;        // this mip's
    uint2 source_size; // the mip before's (mip 0: the scene's)
    uint mip;
    float near_m;      // the camera's: a reverse-Z depth is near_m over the view depth
    float spread_m;    // how much farther than the nearest of four a texel may be and still count fully
    uint reserved;
};

[[vk::push_constant]]
ConstantBuffer<AoDepthPush> push;

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
    if (push.mip == 0)
    {
        const float depth = load_texture(push.depth, int2(texel)).r;
        output[texel] = float4(depth > 0.0 ? push.near_m / depth : kAoFarM, 0.0, 0.0, 0.0);
        return;
    }
    RWTexture2D<float4> source = storage_2d(push.depth);
    const uint2 last = push.source_size - 1u;
    float depths[4];
    float nearest = kAoFarM;
    for (uint i = 0; i < 4; ++i)
    {
        depths[i] = source[min(texel * 2u + uint2(i & 1u, i >> 1u), last)].r;
        nearest = min(nearest, depths[i]);
    }
    float sum = 0.0;
    float weight = 0.0;
    // A plane seen at a slant spreads the four by as much as a fair share of their depth: that much is no edge.
    const float spread = max(push.spread_m, nearest * 0.25 * float(1u << push.mip));
    for (uint i = 0; i < 4; ++i)
    {
        const float counts = saturate(2.0 - (depths[i] - nearest) / max(spread, 1e-4));
        sum += depths[i] * counts;
        weight += counts;
    }
    output[texel] = float4(sum / max(weight, 1e-4), 0.0, 0.0, 0.0);
}
