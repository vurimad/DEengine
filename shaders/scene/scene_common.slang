// What the scene passes share: the frame's constants, the vertex and material layouts (mirrors of
// the C++ structs), vertex decoding and the G-buffer's packing.

import common.fog;

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
    public float4 shadow_color;     // rgb: what shadowed sunlight keeps
    // Sun shadows: kCascadeCount cascades, each a tile of the shadow atlas.
    public float4 cascade_view_proj[16]; // 4 rows per cascade: render-origin space to the cascade's clip space
    public float4 cascade_far;           // per cascade: its far edge, as distance along the view
    public float4 cascade_texel_m;       // per cascade: a shadow texel's size in meters
    public float4 cascade_depth_per_m;   // per cascade: depth units per meter toward the sun
    public float4 camera_forward;        // xyz: the view direction
    public float4 shadow_params;         // x: blend band (fraction of a cascade), y: atlas texel (uv), z: penumbra scale, w: max penumbra (texels)
    public FogMedium fog;
};

public static const uint kCascadeCount = 4;

// A cascade's tile in the 2x2 shadow atlas: uv offset (each tile is half the atlas).
public float2 cascade_tile(uint cascade)
{
    return float2(float(cascade & 1u), float(cascade >> 1u)) * 0.5;
}

// Shaders read the frame through its pointer, never a copy: a copy indexed by cascade would sit in
// scratch memory.
public float4 cascade_clip(SceneFrame* frame, uint cascade, float3 position)
{
    const float4 p = float4(position, 1.0);
    return float4(dot(frame.cascade_view_proj[cascade * 4 + 0], p), dot(frame.cascade_view_proj[cascade * 4 + 1], p),
                  dot(frame.cascade_view_proj[cascade * 4 + 2], p), dot(frame.cascade_view_proj[cascade * 4 + 3], p));
}

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

// A triangle covering the screen, for full-screen passes: draw(3). ndc in [-1, 1], +Y up.
public float4 fullscreen_triangle(uint vertex_id, out float2 ndc)
{
    const float2 corner = float2(float((vertex_id << 1) & 2), float(vertex_id & 2));
    ndc = corner * 2.0 - 1.0;
    return float4(ndc, 0.0, 1.0);
}

// The largest finite value an RGBA16 float target holds, with a little room.
public static const float kHalfMax = 60000.0;

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

// GB1's normal: octahedral, remapped to 0..1.
public float3 gbuffer_normal(float2 gb1)
{
    return octahedral_decode(gb1 * 2.0 - 1.0);
}

// A pixel's position in render-origin space, from its depth (reverse Z; 0 is the far plane).
public float3 position_from_depth(SceneFrame* frame, float2 ndc, float depth)
{
    const float4 point = mul_rows(frame.inv_view_proj, float4(ndc, max(depth, 1e-7), 1.0));
    return point.xyz / point.w;
}

// The first cascade reaching a distance along the view; kCascadeCount past the last.
public uint cascade_at(SceneFrame* frame, float distance)
{
    for (uint c = 0; c < kCascadeCount; ++c)
    {
        if (distance < frame.cascade_far[c])
        {
            return c;
        }
    }
    return kCascadeCount;
}

// How far a distance is into a cascade's blend band, at its far edge: 0 before it, 1 at the edge.
public float cascade_band(SceneFrame* frame, uint cascade, float distance)
{
    const float start = cascade == 0 ? 0.0 : frame.cascade_far[cascade - 1];
    const float band_start = lerp(start, frame.cascade_far[cascade], 1.0 - frame.shadow_params.x);
    return saturate((distance - band_start) / max(frame.cascade_far[cascade] - band_start, 1e-4));
}

// A cascade's uv in the atlas, kept inside its tile (a texel of border) so filters don't bleed.
public float2 atlas_uv(SceneFrame* frame, uint cascade, float2 tile_uv)
{
    const float border = frame.shadow_params.y * 2.0;
    return cascade_tile(cascade) + clamp(tile_uv, border, 1.0 - border) * 0.5;
}

// A froxel column's ray through the middle of its tile (grid_size columns across and down).
public struct FroxelRay
{
    public float3 direction;    // unit
    public float view_per_ray; // view depth per meter along the ray
};

public FroxelRay froxel_ray(SceneFrame* frame, uint2 column, uint2 grid_size)
{
    const float2 ndc = float2((float(column.x) + 0.5) / float(grid_size.x) * 2.0 - 1.0,
                              1.0 - (float(column.y) + 0.5) / float(grid_size.y) * 2.0);
    FroxelRay ray;
    // From a point 100 m or so out, not the near plane: the difference from the camera stays precise
    // far from the render origin.
    ray.direction = normalize(position_from_depth(frame, ndc, 1e-3) - frame.camera_position.xyz);
    ray.view_per_ray = max(dot(ray.direction, frame.camera_forward.xyz), 1e-3);
    return ray;
}

public GBufferSample unpack_gbuffer(float4 gb0, float2 gb1, float4 gb2)
{
    GBufferSample s;
    s.base_color = gb0.rgb;
    s.lighting_group = uint(gb0.a * 255.0 + 0.5);
    s.normal = gbuffer_normal(gb1);
    s.roughness = gb2.r;
    s.metalness = gb2.g;
    s.occlusion = gb2.b;
    s.translucency = gb2.a;
    return s;
}
