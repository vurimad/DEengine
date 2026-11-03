// The scene's placeholder pass (renderer step 1): every mesh instance, lit by one sun and a flat
// ambient, straight into the target. Step 2 replaces it with the G-buffer and deferred lighting.
// Vertices are pulled by address; materials come from the GpuAssets table by index.

import common.bindless;

// Matches render::SceneFrame (engine/render/scene/scene_renderer.cpp).
struct SceneFrame
{
    float4 view_proj[4]; // rows, relative to the render origin
    float4 sun_direction; // xyz: toward the sun
    float4 sun_color;
    float4 ambient_color;
};

// Matches asset::MeshVertex.
struct MeshVertex
{
    float3 position;
    uint normal;  // octahedral, two snorm16
    uint tangent; // snorm8 x, y, z, w (bitangent sign)
    float2 uv;
    uint color;
};

// Matches render::GpuMaterial.
struct GpuMaterial
{
    float4 base_color;
    float3 emissive;
    float metallic;
    float roughness;
    float normal_scale;
    float occlusion_strength;
    float alpha_cutoff;
    uint base_color_texture;
    uint normal_texture;
    uint metallic_roughness_texture;
    uint occlusion_texture;
    uint emissive_texture;
    uint flags;
    uint2 reserved;
};

static const uint kMaterialMasked = 1u;
static const uint kMaterialDoubleSided = 2u;

struct BasicLitPush
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
ConstantBuffer<BasicLitPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 normal : NORMAL;
    float4 tangent : TANGENT;
    float2 uv : TEXCOORD;
};

float2 snorm16x2(uint packed)
{
    const int2 bits = int2(int(packed << 16) >> 16, int(packed) >> 16);
    return max(float2(bits) / 32767.0, -1.0);
}

float4 snorm8x4(uint packed)
{
    const int4 bits = int4(int(packed << 24) >> 24, int(packed << 16) >> 24, int(packed << 8) >> 24, int(packed) >> 24);
    return max(float4(bits) / 127.0, -1.0);
}

float3 octahedral_decode(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    }
    return normalize(n);
}

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    const MeshVertex vertex = push.vertices[vertex_id];
    const float4 row0 = push.instances[push.instance * 3 + 0];
    const float4 row1 = push.instances[push.instance * 3 + 1];
    const float4 row2 = push.instances[push.instance * 3 + 2];
    const float4 local = float4(vertex.position, 1.0);
    const float4 world = float4(dot(row0, local), dot(row1, local), dot(row2, local), 1.0);
    // Normals by the inverse transpose would be exact under non-uniform scale; the 3x3 is close
    // enough for a placeholder (renormalized below).
    const float3x3 linear = float3x3(row0.xyz, row1.xyz, row2.xyz);
    const float3 normal = octahedral_decode(snorm16x2(vertex.normal));
    const float4 tangent = snorm8x4(vertex.tangent);

    const SceneFrame frame = *push.frame;
    VertexOutput output;
    output.position = float4(dot(frame.view_proj[0], world), dot(frame.view_proj[1], world),
                             dot(frame.view_proj[2], world), dot(frame.view_proj[3], world));
    output.normal = mul(linear, normal);
    output.tangent = float4(mul(linear, tangent.xyz), tangent.w);
    output.uv = vertex.uv;
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input, bool front_face: SV_IsFrontFace) : SV_Target
{
    const GpuMaterial material = push.materials[push.material];
    const float4 base = sample_texture(material.base_color_texture, push.sampler, input.uv) * material.base_color;
    if ((material.flags & kMaterialMasked) != 0 && base.a < material.alpha_cutoff)
    {
        discard;
    }
    // Tangent-space normal from the normal map's x, y (z rebuilt), into world space.
    float2 xy = sample_texture(material.normal_texture, push.sampler, input.uv).xy * 2.0 - 1.0;
    xy *= material.normal_scale;
    const float3 tangent_normal = float3(xy, sqrt(saturate(1.0 - dot(xy, xy))));
    float3 n = normalize(input.normal);
    if ((material.flags & kMaterialDoubleSided) != 0 && !front_face)
    {
        n = -n;
    }
    const float3 t = normalize(input.tangent.xyz - n * dot(n, input.tangent.xyz));
    const float3 b = cross(n, t) * input.tangent.w;
    n = normalize(t * tangent_normal.x + b * tangent_normal.y + n * tangent_normal.z);

    const float occlusion =
        lerp(1.0, sample_texture(material.occlusion_texture, push.sampler, input.uv).r, material.occlusion_strength);
    const float3 emissive = sample_texture(material.emissive_texture, push.sampler, input.uv).rgb * material.emissive;
    const SceneFrame frame = *push.frame;
    const float sun = saturate(dot(n, frame.sun_direction.xyz));
    const float3 lit = base.rgb * (frame.sun_color.rgb * sun + frame.ambient_color.rgb * occlusion) + emissive;
    return float4(lit, 1.0);
}
