// Test shader: writes a gradient into a storage texture by bindless index.

import common.bindless;

struct WriteStoragePush
{
    uint texture;
    uint width;
    uint height;
};

[[vk::push_constant]]
ConstantBuffer<WriteStoragePush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    if (thread.x >= push.width || thread.y >= push.height)
    {
        return;
    }
    const float2 position = float2(thread.xy) / float2(push.width, push.height);
    storage_2d(push.texture)[thread.xy] = float4(position, 0.25, 1.0);
}
