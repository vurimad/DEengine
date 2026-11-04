// The G-buffer pass: every mesh instance writes its surface (base color, normal,
// roughness, metalness, occlusion, translucency, lighting group) and its emissive light straight
// into the HDR target. Vertices are pulled by address; materials come from the GpuAssets table.

import common.bindless;
import scene.scene_common;

struct GBufferPush
{
    SceneFrame* frame;
    float4* instances; // kInstanceRows per instance: object to render-origin space, now and last frame
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
    float4 clip : CLIP;                   // where the vertex is, and was last frame: the motion vector
    float4 previous_clip : PREVIOUS_CLIP;
};

struct GBufferOutput
{
    float4 gb0 : SV_Target0;
    float2 gb1 : SV_Target1;
    float4 gb2 : SV_Target2;
    float4 emissive : SV_Target3;
    float2 motion : SV_Target4; // uv now minus uv last frame
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    const MeshVertex vertex = push.vertices[vertex_id];
    const float4 row0 = push.instances[push.instance * kInstanceRows + 0];
    const float4 row1 = push.instances[push.instance * kInstanceRows + 1];
    const float4 row2 = push.instances[push.instance * kInstanceRows + 2];
    const float4 local = float4(vertex.position, 1.0);
    const float4 world = float4(dot(row0, local), dot(row1, local), dot(row2, local), 1.0);
    // The inverse transpose would be exact under non-uniform scale; renormalized in the pixel
    // shader, the 3x3 is close for the uniform scales cells use.
    const float3x3 linear = float3x3(row0.xyz, row1.xyz, row2.xyz);

    VertexOutput output;
    output.position = mul_rows(push.frame.view_proj, world);
    output.clip = output.position;
    const float4 previous_world = float4(dot(push.instances[push.instance * kInstanceRows + 3], local),
                                         dot(push.instances[push.instance * kInstanceRows + 4], local),
                                         dot(push.instances[push.instance * kInstanceRows + 5], local), 1.0);
    output.previous_clip = mul_rows(push.frame.previous_view_proj, previous_world);
    output.normal = mul(linear, octahedral_decode(snorm16x2(vertex.normal)));
    const float4 tangent = snorm8x4(vertex.tangent);
    output.tangent = float4(mul(linear, tangent.xyz), tangent.w);
    output.uv = vertex.uv;
    return output;
}

// The surface into the G-buffer, past any alpha test.
GBufferOutput write_surface(VertexOutput input, bool front_face, GpuMaterial material, float3 base)
{
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
    output.gb0 = float4(base, 0.0); // lighting group 0 until materials carry one
    output.gb1 = octahedral_encode(n) * 0.5 + 0.5;
    output.gb2 = float4(saturate(orm.g * material.roughness), saturate(orm.b * material.metallic), occlusion, 0.0);
    output.emissive = float4(emissive, 1.0);
    // Clip y is up, uv's v is down. Behind last frame's camera there is no previous position: a motion
    // off the screen, so its history is rejected. Bounded either way: a half float holds it, and
    // nothing moves farther than off the screen.
    const float2 ndc = input.clip.xy / input.clip.w;
    const float2 previous_ndc = input.previous_clip.xy / max(input.previous_clip.w, 1e-4);
    output.motion = input.previous_clip.w > 1e-4 ? clamp((ndc - previous_ndc) * float2(0.5, -0.5), -2.0, 2.0) : float2(2.0);
    return output;
}

// Opaque materials: no discard anywhere in the shader, so depth is tested before it runs.
[shader("fragment")]
GBufferOutput fragment_opaque(VertexOutput input, bool front_face: SV_IsFrontFace)
{
    const GpuMaterial material = push.materials[push.material];
    const float3 base = sample_texture(material.base_color_texture, push.sampler, input.uv).rgb * material.base_color.rgb;
    return write_surface(input, front_face, material, base);
}

// Masked materials (the renderer picks this one by their flag): the alpha test.
[shader("fragment")]
GBufferOutput fragment_masked(VertexOutput input, bool front_face: SV_IsFrontFace)
{
    const GpuMaterial material = push.materials[push.material];
    const float4 base = sample_texture(material.base_color_texture, push.sampler, input.uv) * material.base_color;
    if (base.a < material.alpha_cutoff)
    {
        discard;
    }
    return write_surface(input, front_face, material, base.rgb);
}
