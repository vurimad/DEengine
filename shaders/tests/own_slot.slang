// Test shader: binds a buffer to its own descriptor slot, which the engine's pipelines don't
// have; the shader library must refuse to build it.

RWStructuredBuffer<uint> output;

[shader("compute")]
[numthreads(1, 1, 1)]
void compute_main()
{
    output[0] = 1;
}
