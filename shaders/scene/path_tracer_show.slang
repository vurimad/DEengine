// The path tracer's mean so far (scene.path_tracer), as the lit picture: into the HDR target in its place, for the
// exposure and the tonemap to take as they take the rasterized one.

import common.bindless;
import scene.scene_common;

struct PathTracerShowPush
{
    uint accumulation; // sampled: rgb the mean
};

[[vk::push_constant]]
ConstantBuffer<PathTracerShowPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    VertexOutput output;
    output.position = fullscreen_triangle(vertex_id, output.ndc);
    return output;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    return float4(min(load_texture(push.accumulation, int2(input.position.xy)).rgb, kHalfMax), 1.0);
}
