// Test shader: writes value + index into a buffer reached through its device address.

struct FillBufferPush
{
    uint* output;
    uint value;
    uint count;
};

[[vk::push_constant]]
ConstantBuffer<FillBufferPush> push;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    if (thread.x < push.count)
    {
        push.output[thread.x] = push.value + thread.x;
    }
}
