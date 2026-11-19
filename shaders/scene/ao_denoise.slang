// The ambient occlusion's denoise (docs/design.md, Global illumination: GTAO), in compute: scene.gtao's slices turn
// from pixel to pixel, so each pixel holds a few directions and its neighbours the rest. This averages a pixel with
// those of its 3 x 3 that lie on the same surface (by their depth along the view against what the pixel's
// neighbours across it say the surface does there), twice: the second pass with its taps two apart, so together
// they reach five pixels each way without blurring across an edge.

import common.bindless;
import scene.scene_common;

struct AoDenoisePush
{
    uint input;  // the occlusion so far (sampled)
    uint output; // as a storage texture
    uint depth;  // scene.ao_depth's pyramid: mip 0
    uint spacing; // between taps, in pixels
    uint2 size;
    float tolerance; // how far off a surface a tap may be, as a share of the depth, and still count fully
    uint reserved;
};

[[vk::push_constant]]
ConstantBuffer<AoDenoisePush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const int2 texel = int2(thread.xy);
    if (any(thread.xy >= push.size))
    {
        return;
    }
    const int2 last = int2(push.size) - 1;
    const int spacing = int(push.spacing);
    const float depth = load_texture(push.depth, texel).r;
    // The surface's slope across and down, from the nearer-agreeing neighbour each way: a tap is compared with
    // where the surface would be there, so a floor seen at a slant isn't taken for an edge.
    const float left = load_texture(push.depth, clamp(texel - int2(spacing, 0), int2(0), last)).r;
    const float right = load_texture(push.depth, clamp(texel + int2(spacing, 0), int2(0), last)).r;
    const float above = load_texture(push.depth, clamp(texel - int2(0, spacing), int2(0), last)).r;
    const float below = load_texture(push.depth, clamp(texel + int2(0, spacing), int2(0), last)).r;
    const float slope_x = abs(right - depth) < abs(depth - left) ? right - depth : depth - left;
    const float slope_y = abs(below - depth) < abs(depth - above) ? below - depth : depth - above;
    const float tolerance = max(depth * push.tolerance, 1e-3);
    float sum = 0.0;
    float weight = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            const int2 tap = clamp(texel + int2(x, y) * spacing, int2(0), last);
            const float expected = depth + slope_x * float(x) + slope_y * float(y);
            const float off = abs(load_texture(push.depth, tap).r - expected);
            const float kernel = (x == 0 ? 2.0 : 1.0) * (y == 0 ? 2.0 : 1.0); // a tent: 1 2 1
            const float counts = kernel * saturate(1.0 - off / tolerance);
            sum += load_texture(push.input, tap).r * counts;
            weight += counts;
        }
    }
    storage_2d(push.output)[thread.xy] = float4(sum / max(weight, 1e-4), 0.0, 0.0, 0.0);
}
