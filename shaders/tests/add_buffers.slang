// Test shader: output[i] = input[i] + value, both buffers reached through their device addresses.

struct AddBuffersPush
{
    uint* input;
    uint* output;
    uint value;
    uint count;
};

[[vk::push_constant]]
ConstantBuffer<AddBuffersPush> push;

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    if (thread.x < push.count)
    {
        push.output[thread.x] = push.input[thread.x] + push.value;
    }
}
