// The editor's selection outline, over the finished image: a band of radius_px pixels around what the selection covers
// (its mask: x covered, y seen), solid where the selection is seen and fainter where something stands in front of it.
// Its edge is anti-aliased by the distance to the nearest covered pixel.

import common.bindless;
import common.color;
import scene.scene_common;

struct OutlinePush
{
    uint mask;        // RG8: covered, seen
    float radius_px;  // the band's width (1 to 8)
    uint width;       // of the mask and the target
    uint height;
    uint srgb_target; // the target encodes to sRGB: the colour is given as seen, so it's decoded first
};

[[vk::push_constant]]
ConstantBuffer<OutlinePush> push;

static const float3 kOutlineSrgb = float3(0.98, 0.6, 0.12); // the selection's orange, as it shows on screen
static const float kHiddenAlpha = 0.45;
static const int kMaxRadius = 8;

struct VertexOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    float2 ndc;
    VertexOutput output;
    output.position = fullscreen_triangle(vertex_id, ndc);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    const int2 texel = int2(input.position.xy);
    if (load_texture(push.mask, texel).r > 0.5)
    {
        discard; // inside the selection: the outline goes around it, not over it
    }
    // The nearest covered pixel within the band, and whether any covered pixel near it is seen.
    const int reach = min(int(ceil(push.radius_px)), kMaxRadius);
    const int2 last = int2(int(push.width) - 1, int(push.height) - 1);
    float nearest = 1e6;
    float seen = 0.0;
    for (int y = -reach; y <= reach; ++y)
    {
        for (int x = -reach; x <= reach; ++x)
        {
            const float distance = length(float2(x, y));
            if (distance > push.radius_px + 0.5)
            {
                continue;
            }
            const float2 mask = load_texture(push.mask, clamp(texel + int2(x, y), int2(0, 0), last)).rg;
            if (mask.r > 0.5)
            {
                nearest = min(nearest, distance);
                seen = max(seen, mask.g);
            }
        }
    }
    const float coverage = saturate(push.radius_px + 0.5 - nearest);
    if (coverage <= 0.0)
    {
        discard;
    }
    const float3 color = push.srgb_target != 0 ? srgb_to_linear(kOutlineSrgb) : kOutlineSrgb;
    return float4(color, coverage * lerp(kHiddenAlpha, 1.0, seen));
}
