// Test shader: writes what an indirect draw or dispatch reads. mode 0: a count, then draws of a column each
// (tests.indirect_columns), one per instance index. mode 1: a dispatch of count groups, each writing its
// group's index + 1 to the output.

struct WriteIndirectPush
{
    uint* output;
    uint count;
    uint mode;
};

[[vk::push_constant]]
ConstantBuffer<WriteIndirectPush> push;

[shader("compute")]
[numthreads(1, 1, 1)]
void compute_main()
{
    if (push.mode == 1)
    {
        push.output[0] = push.count;
        push.output[1] = 1;
        push.output[2] = 1;
        return;
    }
    // After the count, at the offset of the next 16 bytes: vertices, instances, first vertex, first instance.
    push.output[0] = push.count;
    for (uint i = 0; i < 16; ++i)
    {
        push.output[4 + i * 4 + 0] = 6;
        push.output[4 + i * 4 + 1] = 1;
        push.output[4 + i * 4 + 2] = 0;
        push.output[4 + i * 4 + 3] = i;
    }
}

[shader("compute")]
[numthreads(1, 1, 1)]
void groups_main(uint3 group: SV_DispatchThreadID)
{
    push.output[group.x] = group.x + 1;
}
