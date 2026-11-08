// Shadow casters: every mesh instance into one cascade's tile of the shadow
// atlas, depth only (reverse Z: nearer the sun is greater). Masked materials alpha-test.

import common.bindless;
import scene.scene_common;
import scene.wind;

struct ShadowCasterPush
{
    SceneFrame* frame;
    GpuInstance* instances;
    MeshVertex* vertices;
    GpuMaterial* materials;
    GpuDraw* draws;
    uint sampler;
    uint cascade;
};

[[vk::push_constant]]
ConstantBuffer<ShadowCasterPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD;
    nointerpolation uint material : MATERIAL;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VulkanVertexID, uint draw_id: SV_VulkanInstanceID)
{
    const MeshVertex vertex = push.vertices[vertex_id];
    const GpuDraw draw = push.draws[draw_id];
    const InstanceTransform transform = push.instances[draw.instance].transform;
    const uint3 origin_steps = push.frame.render_origin_steps.xyz;
    float3 world = instance_point(transform, origin_steps, vertex.position);
    // Moving in the wind as the G-buffer has it, or a plant would shade itself where it isn't.
    if ((push.materials[draw.material].flags & kMaterialWind) != 0u)
    {
        const float3x3 linear = float3x3(transform.row0.linear, transform.row1.linear, transform.row2.linear);
        world += plant_sway(push.frame, instance_point(transform, origin_steps, float3(0.0)), world,
                            normalize(mul(linear, octahedral_decode(snorm16x2(vertex.normal)))), unorm8x4(vertex.color),
                            push.materials[draw.material].wind_bend_m, push.materials[draw.material].wind_flutter_m, false);
    }
    VertexOutput output;
    output.position = cascade_clip(push.frame, push.cascade, world);
    output.uv = vertex.uv;
    output.material = draw.material;
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
    const GpuMaterial material = push.materials[input.material];
    const float alpha = sample_texture(material.base_color_texture, push.sampler, input.uv).a * material.base_color.a;
    if (alpha < material.alpha_cutoff)
    {
        discard;
    }
}
