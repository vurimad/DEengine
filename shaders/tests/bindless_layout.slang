// Test shader: touches every bindless table (so reflection reports each binding) and writes the
// module's constants, so tests can check both against the RHI's.

import common.bindless;

struct BindlessLayoutPush
{
    uint* output;
};

[[vk::push_constant]]
ConstantBuffer<BindlessLayoutPush> push;

[shader("compute")]
[numthreads(1, 1, 1)]
void compute_main()
{
    uint width;
    uint height;
    uint depth;
    uint mips;
    texture_2d(0).GetDimensions(0, width, height, mips);
    uint sum = width;
    texture_2d_array(0).GetDimensions(0, width, height, depth, mips);
    sum += width;
    texture_cube(0).GetDimensions(0, width, height, mips);
    sum += width;
    texture_cube_array(0).GetDimensions(0, width, height, depth, mips);
    sum += width;
    texture_3d(0).GetDimensions(0, width, height, depth, mips);
    sum += width;
    storage_2d(0).GetDimensions(width, height);
    sum += width;
    storage_2d_array(0).GetDimensions(width, height, depth);
    sum += width;
    storage_3d(0).GetDimensions(width, height, depth);
    sum += width;
    storage_2d_uint(0).GetDimensions(width, height);
    sum += width;
    sum += uint(texture_2d(0).SampleLevel(sampler_state(0), float2(0.5, 0.5), 0).x);
    if (sum == 123456u)
    {
        // Never runs (comparing needs a depth format); it makes the comparison table part of the program.
        sum += uint(texture_2d(0).SampleCmpLevelZero(comparison_sampler(kSamplerShadowCompare), float2(0.5, 0.5), 0.5));
    }

    push.output[0] = kTextureCapacity;
    push.output[1] = kStorageCapacity;
    push.output[2] = kSamplerCapacity;
    push.output[3] = kEntryBits;
    push.output[4] = sum; // each fallback is 1 texel wide: 9 widths, plus magenta's red
    push.output[5] = DE_SHADER_CHECKS;
}
