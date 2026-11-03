// HDR to the display (renderer step 2): exposure, then a filmic curve (the ACES fit), written to
// an sRGB target (the hardware encodes). Debug views show a G-buffer channel instead.

import common.bindless;
import scene.scene_common;

struct TonemapPush
{
    uint hdr;
    uint gb0;
    uint gb1;
    uint gb2;
    float exposure; // multiplier: 2^EV
    uint view;      // 0 lit, 1 base color, 2 normal, 3 roughness, 4 metalness, 5 occlusion, 6 translucency
};

[[vk::push_constant]]
ConstantBuffer<TonemapPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    const float2 corner = float2(float((vertex_id << 1) & 2), float(vertex_id & 2));
    VertexOutput output;
    output.position = float4(corner * 2.0 - 1.0, 0.0, 1.0);
    return output;
}

// Narkowicz's fit of the ACES filmic curve.
float3 aces(float3 x)
{
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    const int2 texel = int2(input.position.xy);
    if (push.view != 0)
    {
        const GBufferSample s = unpack_gbuffer(load_texture(push.gb0, texel), load_texture(push.gb1, texel).xy,
                                               load_texture(push.gb2, texel));
        switch (push.view)
        {
        case 1: return float4(s.base_color, 1.0);
        case 2: return float4(s.normal * 0.5 + 0.5, 1.0);
        case 3: return float4(s.roughness.xxx, 1.0);
        case 4: return float4(s.metalness.xxx, 1.0);
        case 5: return float4(s.occlusion.xxx, 1.0);
        default: return float4(s.translucency.xxx, 1.0);
        }
    }
    const float3 hdr = load_texture(push.hdr, texel).rgb;
    return float4(aces(hdr * push.exposure), 1.0);
}
