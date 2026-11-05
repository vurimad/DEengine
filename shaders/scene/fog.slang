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
    FogVolumeData* fog_volumes;
    uint2* fog_slice_masks; // per slice, a bit per volume that reaches it
    uint fog_volume_count;
    uint padding;
};

[[vk::push_constant]]
ConstantBuffer<FogPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD;
};

// How much of a pixel's slice's fog to count, when its surface is inside the slice. A slice holds one mean,
// and at distance a slice is as thick as a whole volume: taking a share by depth alone puts the volume's
// fog in front of ground it's really behind, and the other way round, and the error sweeps across the
// volume as slices slide over the ground with the camera. So the volumes' share is what's really in front.
// The world's fog in the slice stays by depth: it's nearly even across one. The volumes' shapes alone are
// enough to place them, without their noise.
float fog_share_in_slice(SceneFrame* frame, float3 direction, float view_per_ray, uint slice, float by_depth, float view_depth)
{
    const uint2 mask = push.fog_slice_masks[slice];
    if ((mask.x | mask.y) == 0)
    {
        return by_depth;
    }
    const float slices = frame.fog.range.w;
    const float reach = frame.fog.range.y;
    const float near_distance = froxel_depth(frame.fog, float(slice) / slices) / view_per_ray;
    const float far_distance = froxel_depth(frame.fog, float(slice + 1) / slices) / view_per_ray;
    const float3 camera = frame.camera_position.xyz;
    const float in_front = fog_volumes_along(push.fog_volumes, mask, 0, camera, direction, view_per_ray, reach, near_distance, view_depth / view_per_ray).a;
    const float through = fog_volumes_along(push.fog_volumes, mask, 0, camera, direction, view_per_ray, reach, near_distance, far_distance).a;
    // As the slice stored it: within where the world's fog starts and ends.
    const float fog_near = clamp(frame.fog.ambient.w, near_distance, far_distance);
    const float fog_far = clamp(frame.fog.range.x, near_distance, far_distance);
    const float world_fog = fog_far > fog_near ? optical_depth(frame.fog, camera.y, direction.y, fog_near, fog_far) : 0.0;
    const float all_fog = through + world_fog;
    return all_fog > 0.0 ? saturate((in_front + world_fog * by_depth) / all_fog) : by_depth;
}

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
        float slice = froxel_t(frame.fog, view_depth) * slices;
        if (push.fog_volume_count > 0 && slice < slices)
        {
            const float first = floor(slice);
            slice = first + fog_share_in_slice(frame, direction, view_per_ray, uint(first), slice - first, view_depth);
        }
        // The grid doesn't move with TAA's offset, and this pixel's ray does.
        const float2 grid_ndc = input.ndc - frame.taa_params.zw;
        const float3 uvw = float3(grid_ndc.x * 0.5 + 0.5, 0.5 - grid_ndc.y * 0.5, (slice - 0.5) / slices);
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
