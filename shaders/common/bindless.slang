// The engine's bindless tables (docs/design.md, Rendering): every sampled texture, storage texture
// and sampler is reached by an index carried in push constants. Indices come from
// Device::texture_index / storage_texture_index; samplers are the DefaultSampler values below.
// Buffers aren't here: shaders reach them through pointers (device addresses).
//
// Each texture shape has its own table, and a texture's one index is valid in the table of the
// shape it was created with: de_textures for a 2D texture, de_cube_textures for a cube...
// Reading it through another shape's table is an error.

// Sampled textures. Sample them in ShaderRead state.
[[vk::binding(0, 0)]]
public Texture2D de_textures[];
[[vk::binding(1, 0)]]
public Texture2DArray de_texture_arrays[];
[[vk::binding(2, 0)]]
public TextureCube de_cube_textures[];
[[vk::binding(3, 0)]]
public TextureCubeArray de_cube_texture_arrays[];
[[vk::binding(4, 0)]]
public Texture3D de_textures_3d[];

// Storage textures, mip 0 (cubes as arrays of faces). Read and write them in ShaderWrite state.
[[vk::binding(5, 0)]]
public RWTexture2D<float4> de_storage_textures[];
[[vk::binding(6, 0)]]
public RWTexture2DArray<float4> de_storage_texture_arrays[];
[[vk::binding(7, 0)]]
public RWTexture3D<float4> de_storage_textures_3d[];

// Samplers. Every default sampler is in both tables at its index; use kSamplerShadowCompare
// through de_comparison_samplers and the others through de_samplers.
[[vk::binding(8, 0)]]
public SamplerState de_samplers[];
[[vk::binding(9, 0)]]
public SamplerComparisonState de_comparison_samplers[];

// rhi::DefaultSampler, by value.
public static const uint kSamplerLinearWrap = 0;
public static const uint kSamplerLinearClamp = 1;
public static const uint kSamplerPointWrap = 2;
public static const uint kSamplerPointClamp = 3;
public static const uint kSamplerAnisotropicWrap = 4;
public static const uint kSamplerAnisotropicClamp = 5;
public static const uint kSamplerShadowCompare = 6;

// Entry 0 of de_textures: 1x1 magenta, where missing and freed 2D indices point.
public static const uint kFallbackTexture = 0;

// Indices may differ between neighboring pixels or threads (per material, per instance), so
// lookups are marked non-uniform; that costs nothing when they happen to agree.
public float4 sample_texture(uint texture, uint sampler, float2 uv)
{
    return de_textures[NonUniformResourceIndex(texture)].Sample(de_samplers[NonUniformResourceIndex(sampler)], uv);
}

public float4 sample_texture_level(uint texture, uint sampler, float2 uv, float mip)
{
    return de_textures[NonUniformResourceIndex(texture)].SampleLevel(de_samplers[NonUniformResourceIndex(sampler)], uv,
                                                                     mip);
}

public float4 load_texture(uint texture, int2 texel, int mip = 0)
{
    return de_textures[NonUniformResourceIndex(texture)].Load(int3(texel, mip));
}

public uint2 texture_size(uint texture, uint mip = 0)
{
    uint width;
    uint height;
    uint mips;
    de_textures[NonUniformResourceIndex(texture)].GetDimensions(mip, width, height, mips);
    return uint2(width, height);
}
