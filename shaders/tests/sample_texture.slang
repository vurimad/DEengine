// Test shader: samples a texture by bindless index at every texel center and writes the colors
// through a pointer.

import common.bindless;

struct SampleTexturePush
{
    float4* output;
    uint texture;
    uint sampler;
    uint width;
    uint height;
};

[[vk::push_constant]]
ConstantBuffer<SampleTexturePush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    if (thread.x >= push.width || thread.y >= push.height)
    {
        return;
    }
    const float2 uv = (float2(thread.xy) + 0.5) / float2(push.width, push.height);
    push.output[thread.y * push.width + thread.x] = sample_texture_level(push.texture, push.sampler, uv, 0.0);
}
