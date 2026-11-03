// The G-buffer pass (renderer step 2): every mesh instance writes its surface (base color, normal,
// roughness, metalness, occlusion, translucency, lighting group) and its emissive light straight
// into the HDR target. Vertices are pulled by address; materials come from the GpuAssets table.

import common.bindless;
import scene.scene_common;

struct GBufferPush
{
    SceneFrame* frame;
    float4* instances; // 3 rows per instance: object to render-origin space
    MeshVertex* vertices;
    GpuMaterial* materials;
    uint instance;
    uint material;
    uint sampler;
};

[[vk::push_constant]]
ConstantBuffer<GBufferPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD;
};

struct GBufferOutput
{
    float4 gb0 : SV_Target0;
    float2 gb1 : SV_Target1;
    float4 gb2 : SV_Target2;
    float4 emissive : SV_Target3;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    const MeshVertex vertex = push.vertices[vertex_id];
    const float4 row0 = push.instances[push.instance * 3 + 0];
    const float4 row1 = push.instances[push.instance * 3 + 1];
    const float4 row2 = push.instances[push.instance * 3 + 2];
    const float4 local = float4(vertex.position, 1.0);
    const float4 world = float4(dot(row0, local), dot(row1, local), dot(row2, local), 1.0);
    // The inverse transpose would be exact under non-uniform scale; renormalized in the pixel
    // shader, the 3x3 is close for the uniform scales cells use.
    const float3x3 linear = float3x3(row0.xyz, row1.xyz, row2.xyz);

    VertexOutput output;
    output.position = mul_rows(push.frame.view_proj, world);
    output.normal = mul(linear, octahedral_decode(snorm16x2(vertex.normal)));
    const float4 tangent = snorm8x4(vertex.tangent);
    output.tangent = float4(mul(linear, tangent.xyz), tangent.w);
    output.uv = vertex.uv;
    return output;
}

[shader("fragment")]
GBufferOutput fragment_main(VertexOutput input, bool front_face: SV_IsFrontFace)
{
    const GpuMaterial material = push.materials[push.material];
    const float4 base = sample_texture(material.base_color_texture, push.sampler, input.uv) * material.base_color;
    if ((material.flags & kMaterialMasked) != 0 && base.a < material.alpha_cutoff)
    {
        discard;
    }
    // Tangent-space normal from the map's x, y (z rebuilt), into world space.
    float2 xy = sample_texture(material.normal_texture, push.sampler, input.uv).xy * 2.0 - 1.0;
    xy *= material.normal_scale;
    const float3 tangent_normal = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    // The tangent frame from the front face's normal; a back face is the mirror image: the same
    // tangent and bitangent, the normal flipped (flipping it first would flip the bitangent too).
    const float3 geometric = normalize(input.normal);
    const float3 t = normalize(input.tangent.xyz - geometric * dot(geometric, input.tangent.xyz));
    const float3 b = cross(geometric, t) * input.tangent.w;
    const bool back = (material.flags & kMaterialDoubleSided) != 0 && !front_face;
    const float3 n = normalize(t * tangent_normal.x + b * tangent_normal.y +
                               (back ? -geometric : geometric) * tangent_normal.z);

    // glTF's packing: occlusion in r, roughness in g, metalness in b.
    const float3 orm = sample_texture(material.metallic_roughness_texture, push.sampler, input.uv).rgb;
    const float occlusion =
        lerp(1.0, sample_texture(material.occlusion_texture, push.sampler, input.uv).r, material.occlusion_strength);
    const float3 emissive = sample_texture(material.emissive_texture, push.sampler, input.uv).rgb * material.emissive;

    GBufferOutput output;
    output.gb0 = float4(base.rgb, 0.0); // lighting group 0 until materials carry one
    output.gb1 = octahedral_encode(n) * 0.5 + 0.5;
    output.gb2 = float4(saturate(orm.g * material.roughness), saturate(orm.b * material.metallic), occlusion, 0.0);
    output.emissive = float4(emissive, 1.0);
    return output;
}
