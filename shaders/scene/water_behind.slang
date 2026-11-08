// What lies behind the water, kept before it's drawn: the lit image, and the depth (as a color, to be read while the
// water writes the depth target). The water pass reads both: what's seen through it, and how much water is in the way.

import common.bindless;
import scene.scene_common;

struct WaterBehindPush
{
    uint hdr;
    uint depth;
};

[[vk::push_constant]]
ConstantBuffer<WaterBehindPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD;
};

struct Behind
{
    float4 color : SV_Target0;
    float depth : SV_Target1;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    VertexOutput output;
    output.position = fullscreen_triangle(vertex_id, output.ndc);
    return output;
}

[shader("fragment")]
Behind fragment_main(VertexOutput input)
{
    const int2 texel = int2(input.position.xy);
    Behind output;
    output.color = load_texture(push.hdr, texel);
    output.depth = load_texture(push.depth, texel).r;
    return output;
}
