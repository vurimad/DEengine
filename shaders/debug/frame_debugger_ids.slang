// The frame debugger's way to integer textures (engine/tools/frame_debugger.h): IDs, visibility. A fragment shader
// can't read storage textures here, and integer textures are only reached as those, so this copies one mip into a
// float texture the view then shows like any other: each value as a float (exact up to 2^24), in red.

import common.bindless;

// Matches IdsPush in engine/tools/frame_debugger.cpp.
struct IdsPush
{
    uint source; // the integer texture's mip, as a storage texture
    uint target; // the float texture, as a storage texture
    uint width;
    uint height;
};

[[vk::push_constant]]
ConstantBuffer<IdsPush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    if (thread.x >= push.width || thread.y >= push.height)
    {
        return;
    }
    const uint value = storage_2d_uint(push.source)[thread.xy];
    storage_2d(push.target)[thread.xy] = float4(float(value), 0.0, 0.0, 1.0);
}
