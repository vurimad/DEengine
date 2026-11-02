// The engine's bindless tables (docs/design.md, Rendering): every sampled texture, storage texture
// and sampler is reached by an index carried in push constants. Texture indices come from
// Device::texture_index / storage_texture_index; samplers are the DefaultSampler values below.
// Buffers aren't here: shaders reach them through pointers (device addresses).
//
// Each texture shape has its own table (rhi::BindlessTable gives the same binding numbers), and a
// texture's index belongs to the table of its shape. Reach textures through the accessors below,
// never the raw arrays: they keep a wrong index from reaching any texture but the fallback.
//   - The index carries its table in its top bits. An index of another table, or past the end of
//     this one, reads entry 0: the fallback (magenta for sampled tables). One compare, every build.
//   - Every entry not in use is the fallback too, and freed entries are reused last, so a stale
//     index reads the fallback until its table has been used all the way round.

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

// Storage textures, one entry per mip (cubes as arrays of faces). Use them in ShaderWrite state.
[[vk::binding(5, 0)]]
public RWTexture2D<float4> de_storage_textures[];
[[vk::binding(6, 0)]]
public RWTexture2DArray<float4> de_storage_texture_arrays[];
[[vk::binding(7, 0)]]
public RWTexture3D<float4> de_storage_textures_3d[];
[[vk::binding(8, 0)]]
public RWTexture2D<uint> de_storage_textures_uint[];

// Samplers. Every default sampler is in both tables at its index; use kSamplerShadowCompare
// through the comparison table and the others through the plain one.
[[vk::binding(9, 0)]]
public SamplerState de_samplers[];
[[vk::binding(10, 0)]]
public SamplerComparisonState de_comparison_samplers[];

// rhi::BindlessTable, by value.
public static const uint kTableTexture2D = 0;
public static const uint kTableTexture2DArray = 1;
public static const uint kTableTextureCube = 2;
public static const uint kTableTextureCubeArray = 3;
public static const uint kTableTexture3D = 4;
public static const uint kTableStorage2D = 5;
public static const uint kTableStorage2DArray = 6;
public static const uint kTableStorage3D = 7;
public static const uint kTableStorage2DUint = 8;

// rhi::kBindless*Capacity and the index layout.
public static const uint kTextureCapacity = 16384;
public static const uint kStorageCapacity = 4096;
public static const uint kSamplerCapacity = 64;
public static const uint kEntryBits = 24;
public static const uint kEntryMask = (1u << 24) - 1u;

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

// The entry an index names in a table, or 0 (the fallback) for an index of another table or past
// the end of this one.
public uint bindless_entry(uint index, uint table, uint capacity)
{
    const uint entry = index & kEntryMask;
    return ((index >> kEntryBits) == table && entry < capacity) ? entry : 0u;
}

// Indices may differ between neighboring pixels or threads (per material, per instance), so
// lookups are marked non-uniform; that costs nothing when they happen to agree.
public Texture2D texture_2d(uint index)
{
    return de_textures[NonUniformResourceIndex(bindless_entry(index, kTableTexture2D, kTextureCapacity))];
}

public Texture2DArray texture_2d_array(uint index)
{
    return de_texture_arrays[NonUniformResourceIndex(bindless_entry(index, kTableTexture2DArray, kTextureCapacity))];
}

public TextureCube texture_cube(uint index)
{
    return de_cube_textures[NonUniformResourceIndex(bindless_entry(index, kTableTextureCube, kTextureCapacity))];
}

public TextureCubeArray texture_cube_array(uint index)
{
    return de_cube_texture_arrays[NonUniformResourceIndex(
        bindless_entry(index, kTableTextureCubeArray, kTextureCapacity))];
}

public Texture3D texture_3d(uint index)
{
    return de_textures_3d[NonUniformResourceIndex(bindless_entry(index, kTableTexture3D, kTextureCapacity))];
}

public RWTexture2D<float4> storage_2d(uint index)
{
    return de_storage_textures[NonUniformResourceIndex(bindless_entry(index, kTableStorage2D, kStorageCapacity))];
}

public RWTexture2DArray<float4> storage_2d_array(uint index)
{
    return de_storage_texture_arrays[NonUniformResourceIndex(
        bindless_entry(index, kTableStorage2DArray, kStorageCapacity))];
}

public RWTexture3D<float4> storage_3d(uint index)
{
    return de_storage_textures_3d[NonUniformResourceIndex(bindless_entry(index, kTableStorage3D, kStorageCapacity))];
}

public RWTexture2D<uint> storage_2d_uint(uint index)
{
    return de_storage_textures_uint[NonUniformResourceIndex(
        bindless_entry(index, kTableStorage2DUint, kStorageCapacity))];
}

// Samplers past the end read entry 0: LinearWrap, or ShadowCompare's padding in the comparison table.
public SamplerState sampler_state(uint sampler)
{
    return de_samplers[NonUniformResourceIndex(sampler < kSamplerCapacity ? sampler : 0u)];
}

public SamplerComparisonState comparison_sampler(uint sampler)
{
    return de_comparison_samplers[NonUniformResourceIndex(sampler < kSamplerCapacity ? sampler : kSamplerShadowCompare)];
}

// Shorthands for the common 2D cases.
public float4 sample_texture(uint texture, uint sampler, float2 uv)
{
    return texture_2d(texture).Sample(sampler_state(sampler), uv);
}

public float4 sample_texture_level(uint texture, uint sampler, float2 uv, float mip)
{
    return texture_2d(texture).SampleLevel(sampler_state(sampler), uv, mip);
}

public float4 load_texture(uint texture, int2 texel, int mip = 0)
{
    return texture_2d(texture).Load(int3(texel, mip));
}

public uint2 texture_size(uint texture, uint mip = 0)
{
    uint width;
    uint height;
    uint mips;
    texture_2d(texture).GetDimensions(mip, width, height, mips);
    return uint2(width, height);
}

// 1 where the stored depth is at least as near as compare_depth (reverse Z), filtered.
public float sample_shadow(uint texture, float2 uv, float compare_depth)
{
    return texture_2d(texture).SampleCmpLevelZero(comparison_sampler(kSamplerShadowCompare), uv, compare_depth);
}
