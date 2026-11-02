// The sandbox's textured quad: a texture reached by bindless index, drawn without vertex buffers.

import common.bindless;

struct TexturedQuadPush
{
    float2 center;  // clip space
    float2 size;    // clip space
    uint texture;   // bindless index
    uint sampler;   // DefaultSampler
    float time_s;
};

[[vk::push_constant]]
ConstantBuffer<TexturedQuadPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    // Two counter-clockwise triangles; uv (0, 0) is the texture's top-left corner.
    const float2 corners[6] = { float2(-1, -1), float2(1, -1), float2(1, 1),
                                float2(-1, -1), float2(1, 1), float2(-1, 1) };
    const float2 corner = corners[vertex_id];

    VertexOutput output;
    output.position = float4(push.center + corner * push.size * 0.5, 0.5, 1.0);
    output.uv = float2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    // A slow drift, so the sampler's wrap mode shows.
    const float2 uv = input.uv + float2(push.time_s * 0.05, 0.0);
    return sample_texture(push.texture, push.sampler, uv);
}
