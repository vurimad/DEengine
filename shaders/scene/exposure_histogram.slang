// Auto exposure, step 1 (docs/design.md, Auto exposure), in compute: how bright the frame is, as a histogram of its
// pixels' luminance on a log scale. Every other pixel each way (a quarter of them: plenty to meter by), each counted
// more toward the middle of the picture (centre-weighted, as a camera meters: what's looked at matters most). Black
// isn't counted at all: a pixel that sends no light (nothing there, no sky) says nothing of how bright the scene is,
// and counted as the darkest there is, a box in the void came out stops too bright. A group gathers its counts in
// shared memory and adds them to the frame's histogram once.

import common.bindless;

static const uint kBins = 256;

struct ExposureHistogramPush
{
    uint* histogram; // kBins counts, zeroed before this pass
    uint2 size;      // on a multiple of 8, as a uint2 has to be
    uint image;      // the lit picture, before the tonemap
    float min_log2;      // bin 1's lower edge; bin 0 (black) is left empty
    float bins_per_log2; // (kBins - 1) over the range
};

[[vk::push_constant]]
ConstantBuffer<ExposureHistogramPush> push;

groupshared uint bins[kBins];

[shader("compute")]
[numthreads(16, 16, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID, uint index: SV_GroupIndex)
{
    bins[index] = 0;
    GroupMemoryBarrierWithGroupSync();
    const uint2 texel = thread.xy * 2u;
    float3 color = float3(0.0);
    if (all(texel < push.size))
    {
        color = load_texture(push.image, int2(texel)).rgb;
    }
    const float luminance = dot(select(isfinite(color), max(color, 0.0), 0.0), float3(0.2126, 0.7152, 0.0722));
    if (luminance > 1e-8)
    {
        const uint bin =
            uint(clamp((log2(luminance) - push.min_log2) * push.bins_per_log2, 0.0, float(kBins - 2))) + 1u;
        // Four times as much at the middle as at the corners.
        const float2 from_middle = (float2(texel) / float2(push.size) - 0.5) * 2.0;
        const uint weight = 1u + uint(3.0 * saturate(1.0 - length(from_middle) * 0.8) + 0.5);
        InterlockedAdd(bins[bin], weight);
    }
    GroupMemoryBarrierWithGroupSync();
    if (bins[index] != 0)
    {
        InterlockedAdd(push.histogram[index], bins[index]);
    }
}
