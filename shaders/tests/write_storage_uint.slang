// Test shader: writes x + y * width + value into an integer storage texture by bindless index.

import common.bindless;

struct WriteStorageUintPush
{
    uint texture;
    uint width;
    uint height;
    uint value;
};

[[vk::push_constant]]
ConstantBuffer<WriteStorageUintPush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    if (thread.x >= push.width || thread.y >= push.height)
    {
        return;
    }
    storage_2d_uint(push.texture)[thread.xy] = thread.x + thread.y * push.width + push.value;
}
