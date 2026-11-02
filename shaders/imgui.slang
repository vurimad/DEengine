// Dear ImGui's draw lists (engine/render/imgui): vertices read through a pointer, textures by
// bindless index, alpha blended over what the target holds.

import common.bindless;
import common.color;

struct ImGuiPush
{
    uint* vertices;    // ImDrawVert: float2 pos, float2 uv, uint color (RGBA8), 20 bytes, packed
    float2 scale;      // ImGui coordinates (points, y down) to clip space (y up)
    float2 translate;
    uint texture;      // bindless index
    uint sampler;      // DefaultSampler
    uint srgb_target;  // 1: the target encodes sRGB, so ImGui's sRGB colors go in linear
};

[[vk::push_constant]]
ConstantBuffer<ImGuiPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
    float4 color : COLOR;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    // Five 32-bit words per vertex: no struct, so no padding rules get in the way.
    uint* vertex = push.vertices + vertex_id * 5;
    const float2 position = float2(asfloat(vertex[0]), asfloat(vertex[1]));
    const float2 uv = float2(asfloat(vertex[2]), asfloat(vertex[3]));
    const uint packed = vertex[4];
    float4 color = float4(packed & 0xFF, (packed >> 8) & 0xFF, (packed >> 16) & 0xFF, packed >> 24) / 255.0;
    if (push.srgb_target != 0)
    {
        color.rgb = srgb_to_linear(color.rgb);
    }

    VertexOutput output;
    output.position = float4(position * push.scale + push.translate, 0.0, 1.0);
    output.uv = uv;
    output.color = color;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    return input.color * sample_texture(push.texture, push.sampler, input.uv);
}
