// HDR to the display (renderer step 2): exposure, then a filmic curve (the ACES fit), written to
// an sRGB target (the hardware encodes). Debug views show a G-buffer channel instead.

import common.bindless;
import common.color;
import scene.scene_common;

struct TonemapPush
{
    uint hdr;
    uint gb0;
    uint gb1;
    uint gb2;
    float exposure;    // multiplier: 2^EV
    uint view;         // 0 lit, 1 base color, 2 normal, 3 roughness, 4 metalness, 5 occlusion, 6 translucency,
                       // 7 shadow mask, 8 cascades
    uint srgb_target;  // the target encodes to sRGB: data views are decoded first, so they show as stored
    uint shadow_mask;  // r: sunlight seen, g: cascade
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
    float2 ndc;
    VertexOutput output;
    output.position = fullscreen_triangle(vertex_id, ndc);
    return output;
}

// A data value (a normal, a roughness) shown as itself: an sRGB target would encode it brighter.
float4 show_data(float3 value)
{
    return float4(push.srgb_target != 0 ? srgb_to_linear(value) : value, 1.0);
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
    if (push.view >= 7)
    {
        const float2 mask = load_texture(push.shadow_mask, texel).rg;
        if (push.view == 7)
        {
            return show_data(mask.rrr);
        }
        // Cascades: red, green, blue, yellow, darkened where shadowed.
        const float3 colors[4] = { float3(1, 0.3, 0.3), float3(0.3, 1, 0.3), float3(0.3, 0.4, 1), float3(1, 1, 0.3) };
        const uint cascade = min(uint(mask.g * 4.0), 3u);
        return float4(colors[cascade] * (0.35 + 0.65 * mask.r), 1.0);
    }
    if (push.view != 0)
    {
        const GBufferSample s = unpack_gbuffer(load_texture(push.gb0, texel), load_texture(push.gb1, texel).xy,
                                               load_texture(push.gb2, texel));
        switch (push.view)
        {
        case 1: return float4(s.base_color, 1.0);
        case 2: return show_data(s.normal * 0.5 + 0.5);
        case 3: return show_data(s.roughness.xxx);
        case 4: return show_data(s.metalness.xxx);
        case 5: return show_data(s.occlusion.xxx);
        default: return show_data(s.translucency.xxx);
        }
    }
    const float3 hdr = clamp(load_texture(push.hdr, texel).rgb, 0.0, kHalfMax);
    return float4(aces(hdr * push.exposure), 1.0);
}
