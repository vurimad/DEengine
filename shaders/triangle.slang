// The first pixels: a triangle drawn without vertex buffers, its corners cycling through hues.
// Edit it while the engine runs: it rebuilds and swaps in live.

import common.color;

struct TrianglePush
{
    float time_s;
    float aspect; // width / height, so the triangle keeps its shape
};

[[vk::push_constant]]
ConstantBuffer<TrianglePush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float3 color : COLOR;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    // Counter-clockwise in the engine's +Y-up clip space.
    const float2 corners[3] = { float2(0.0, 0.6), float2(-0.6, -0.45), float2(0.6, -0.45) };
    const float2 corner = corners[vertex_id];

    VertexOutput output;
    output.position = float4(corner.x / push.aspect, corner.y, 0.5, 1.0);
    output.color = hue_to_rgb(frac(push.time_s * 0.1 + float(vertex_id) / 3.0));
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    return float4(input.color, 1.0);
}
