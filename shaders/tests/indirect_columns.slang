// Test shader: each instance fills one column of the target with what the draw told it, as bytes: its
// instance index + 1, its draw's index + 1, SV_InstanceID + 1, and SV_VertexID / 6 + 1, which says what a
// draw's first vertex and vertex offset do to it. Six vertices make a column.

struct IndirectColumnsPush
{
    uint width;
};

[[vk::push_constant]]
ConstantBuffer<IndirectColumnsPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    nointerpolation uint4 told : TOLD;
};

[shader("vertex")]
VertexOutput vertex_main(uint vulkan_vertex: SV_VulkanVertexID, uint vulkan_instance: SV_VulkanInstanceID, uint draw: SV_DrawIndex,
                         uint vertex_id: SV_VertexID, uint instance_id: SV_InstanceID)
{
    const float2 corners[6] = { float2(0.0, 1.0), float2(0.0, 0.0), float2(1.0, 0.0), float2(0.0, 1.0), float2(1.0, 0.0), float2(1.0, 1.0) };
    const float2 corner = corners[vulkan_vertex % 6];
    VertexOutput output;
    output.position = float4((float(vulkan_instance) + corner.x) / float(push.width) * 2.0 - 1.0, corner.y * 2.0 - 1.0, 0.5, 1.0);
    output.told = uint4(vulkan_instance + 1, draw + 1, instance_id + 1, vertex_id / 6 + 1);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    return float4(input.told) / 255.0;
}
