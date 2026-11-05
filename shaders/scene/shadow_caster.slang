// Shadow casters: every mesh instance into one cascade's tile of the shadow
// atlas, depth only (reverse Z: nearer the sun is greater). Masked materials alpha-test.

import common.bindless;
import scene.scene_common;

struct ShadowCasterPush
{
    SceneFrame* frame;
    float4* instances; // 3 rows per instance: object to render-origin space
    MeshVertex* vertices;
    GpuMaterial* materials;
    uint instance;
    uint material;
    uint sampler;
    uint cascade;
};

[[vk::push_constant]]
ConstantBuffer<ShadowCasterPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VulkanVertexID)
{
    const MeshVertex vertex = push.vertices[vertex_id];
    const float4 local = float4(vertex.position, 1.0);
    const float3 world = float3(dot(push.instances[push.instance * kInstanceRows + 0], local),
                                dot(push.instances[push.instance * kInstanceRows + 1], local),
                                dot(push.instances[push.instance * kInstanceRows + 2], local));
    VertexOutput output;
    output.position = cascade_clip(push.frame, push.cascade, world);
    output.uv = vertex.uv;
    return output;
}

// Opaque casters: depth only. Nothing to do, and no discard: early depth testing stays on.
[shader("fragment")]
void fragment_opaque(VertexOutput input)
{
}

// Masked casters (the renderer picks this one by their flag): the alpha test.
[shader("fragment")]
void fragment_masked(VertexOutput input)
{
    const GpuMaterial material = push.materials[push.material];
    const float alpha = sample_texture(material.base_color_texture, push.sampler, input.uv).a * material.base_color.a;
    if (alpha < material.alpha_cutoff)
    {
        discard;
    }
}
