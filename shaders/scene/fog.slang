// Fog: one full-screen pass over the lit HDR image, into a new one. Each pixel's light is dimmed by
// the fog between it and the camera and the fog's own light is added (common.fog). Where nothing
// was drawn, the sky, as far as the fog reaches: the horizon takes the haze of the land below it.

import common.bindless;
import common.fog;
import scene.scene_common;

struct FogPush
{
    SceneFrame* frame;
    uint hdr;   // lit, unfogged
    uint depth;
};

[[vk::push_constant]]
ConstantBuffer<FogPush> push;

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
    SceneFrame* frame = push.frame;
    const int2 texel = int2(input.position.xy);
    const float3 color = load_texture(push.hdr, texel).rgb;
    const float depth = load_texture(push.depth, texel).r;
    // The point (on the far plane for the sky: only its direction counts), and how far it is.
    const float3 to_point = position_from_depth(frame, input.ndc, depth) - frame.camera_position.xyz;
    const float distance = depth > 0.0 ? length(to_point) : 1e30;
    const FogResult fog = fog_along_ray(frame.fog, frame.camera_position.xyz, normalize(to_point), distance,
                                        frame.sun_direction.xyz, frame.sun_color.rgb);
    return float4(min(color * fog.transmittance + fog.in_scatter, kHalfMax), 1.0);
}
