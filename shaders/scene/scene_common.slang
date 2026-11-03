// What the scene passes share: the frame's constants, the vertex and material layouts (mirrors of
// the C++ structs), vertex decoding and the G-buffer's packing.

// Matches render::SceneFrame (engine/render/scene/scene_renderer.cpp).
public struct SceneFrame
{
    public float4 view_proj[4];     // rows, relative to the render origin
    public float4 inv_view_proj[4]; // rows: clip back to render-origin space
    public float4 camera_position;  // xyz, relative to the render origin
    public float4 sun_direction;    // xyz: toward the sun
    public float4 sun_color;        // rgb: illuminance, in exposure-relative units
    public float4 sky_color;        // rgb: ambient from above
    public float4 ground_color;     // rgb: ambient from below
    public float4 shadow_color;     // rgb: what shadowed sunlight keeps (no shadows yet: unused)
};

// Matches asset::MeshVertex.
public struct MeshVertex
{
    public float3 position;
    public uint normal;  // octahedral, two snorm16
    public uint tangent; // snorm8 x, y, z, w (bitangent sign)
    public float2 uv;
    public uint color;
};

// Matches render::GpuMaterial (engine/render/scene/gpu_assets.h).
public struct GpuMaterial
{
    public float4 base_color;
    public float3 emissive;
    public float metallic;
    public float roughness;
    public float normal_scale;
    public float occlusion_strength;
    public float alpha_cutoff;
    public uint base_color_texture;
    public uint normal_texture;
    public uint metallic_roughness_texture;
    public uint occlusion_texture;
    public uint emissive_texture;
    public uint flags;
    public uint2 reserved;
};

public static const uint kMaterialMasked = 1u;
public static const uint kMaterialDoubleSided = 2u;

public float2 snorm16x2(uint packed)
{
    const int2 bits = int2(int(packed << 16) >> 16, int(packed) >> 16);
    return max(float2(bits) / 32767.0, -1.0);
}

public float4 snorm8x4(uint packed)
{
    const int4 bits = int4(int(packed << 24) >> 24, int(packed << 16) >> 24, int(packed << 8) >> 24, int(packed) >> 24);
    return max(float4(bits) / 127.0, -1.0);
}

// Octahedral normals: a unit vector as two values in [-1, 1].
public float3 octahedral_decode(float2 e)
{
    float3 n = float3(e, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    }
    return normalize(n);
}

public float2 octahedral_encode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * select(n.xy >= 0.0, 1.0, -1.0);
    }
    return n.xy;
}

public float4 mul_rows(float4 rows[4], float4 v)
{
    return float4(dot(rows[0], v), dot(rows[1], v), dot(rows[2], v), dot(rows[3], v));
}

// The G-buffer (docs/design.md, Rendering: G-buffer v1):
//   0 RGBA8 sRGB  base color, lighting group / 255
//   1 RG16 unorm  octahedral world normal, remapped to 0..1
//   2 RGBA8 unorm roughness, metalness, occlusion, translucency
//   + emissive straight into the HDR target, and reverse-Z depth
public struct GBufferSample
{
    public float3 base_color;
    public uint lighting_group;
    public float3 normal;
    public float roughness;
    public float metalness;
    public float occlusion;
    public float translucency;
};

public GBufferSample unpack_gbuffer(float4 gb0, float2 gb1, float4 gb2)
{
    GBufferSample s;
    s.base_color = gb0.rgb;
    s.lighting_group = uint(gb0.a * 255.0 + 0.5);
    s.normal = octahedral_decode(gb1 * 2.0 - 1.0);
    s.roughness = gb2.r;
    s.metalness = gb2.g;
    s.occlusion = gb2.b;
    s.translucency = gb2.a;
    return s;
}
