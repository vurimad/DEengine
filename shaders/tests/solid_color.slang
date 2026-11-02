// Test shader: one counter-clockwise triangle over the middle of the target, in a pushed color.

struct SolidColorPush
{
    float4 color;
};

[[vk::push_constant]]
ConstantBuffer<SolidColorPush> push;

[shader("vertex")]
float4 vertex_main(uint vertex_id: SV_VertexID) : SV_Position
{
    const float2 corners[3] = { float2(0.0, 0.8), float2(-0.8, -0.8), float2(0.8, -0.8) };
    return float4(corners[vertex_id], 0.5, 1.0);
}

[shader("fragment")]
float4 fragment_main() : SV_Target
{
    return push.color;
}
