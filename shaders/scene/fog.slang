// Fog: one full-screen pass over the lit HDR image, into a new one. Each pixel's light is dimmed by
// the fog between it and the camera and the fog's own light is added: from the froxel grid as far as
// it reaches (fog_integrate: with the sun's shadows), and past it in closed form (common.fog). Where
// nothing was drawn, the sky, as far as the fog reaches: the horizon takes the haze of the land below it.

import common.bindless;
import common.fog;
import scene.scene_common;

struct FogPush
{
    SceneFrame* frame;
    uint hdr;        // lit, unfogged
    uint depth;
    uint integrated; // the froxel grid, integrated: (in-scatter, optical depth); unused without one
    uint sampler;    // linear clamp
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
    const float3 camera = frame.camera_position.xyz;
    const float3 to_point = position_from_depth(frame, input.ndc, depth) - camera;
    const float3 direction = normalize(to_point);
    const float distance = depth > 0.0 ? length(to_point) : 1e30;
    const float3 sun_direction = frame.sun_direction.xyz;
    const float3 sun_color = frame.sun_color.rgb;

    // The grid, up to the point or its reach.
    const float reach = frame.fog.range.y;
    float3 near_transmittance = float3(1.0);
    float3 near_scatter = float3(0.0);
    float from_distance = 0.0;
    if (reach > 0.0)
    {
        const float view_per_ray = max(dot(direction, frame.camera_forward.xyz), 1e-3);
        const float view_depth = min(distance * view_per_ray, reach);
        // Slice k holds the integral through its far edge, at the texture's k + 0.5. Inside the first
        // slice, a share of it: the integral starts at nothing.
        const float slices = frame.fog.range.w;
        const float slice = froxel_t(frame.fog, view_depth) * slices;
        const float3 uvw = float3(input.ndc.x * 0.5 + 0.5, 0.5 - input.ndc.y * 0.5, (slice - 0.5) / slices);
        const float4 grid = texture_3d(push.integrated).SampleLevel(sampler_state(push.sampler), uvw, 0.0) * saturate(slice);
        near_transmittance = exp(-grid.a * max(frame.fog.extinction.rgb, float3(0.01)));
        near_scatter = grid.rgb;
        from_distance = reach / view_per_ray;
    }
    // Then the rest of the way, in closed form.
    const FogResult far = fog_between(frame.fog, camera, direction, from_distance, distance, sun_direction, sun_color);
    const float3 result = color * near_transmittance * far.transmittance + near_scatter + near_transmittance * far.in_scatter;
    return float4(min(result, kHalfMax), 1.0);
}
