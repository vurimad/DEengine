// What's drawn after the deferred light and the fog shares (docs/design.md, Rendering: frame passes, step 10): the sun
// through its shadow cascades at a point, and the fog between the camera and a point. The transparent pass asks for
// both at each pixel (scene.transparent); particles once per particle (scene.particles).

import common.bindless;
import common.fog;
import scene.scene_common;

// How much of the sun reaches a point at a view depth, as the fog's grid asks (scene.fog_inject): one filtered sample
// of the first cascade reaching that far, the receiver pushed off its surface by about a texel (a normal of zero: only
// toward the sun, for a point with no surface).
public float sun_visibility(SceneFrame* frame, uint atlas, float3 position, float3 normal, float view_depth)
{
    const uint cascade = cascade_at(frame, view_depth);
    if (cascade == kCascadeCount)
    {
        return 1.0;
    }
    const float3 receiver = position + (normal + frame.sun_direction.xyz) * (frame.cascade_texel_m[cascade] * 1.5);
    const float4 clip = cascade_clip(frame, cascade, receiver);
    const float2 tile_uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    const float seen = sample_shadow(atlas, atlas_uv(frame, cascade, tile_uv), clip.z);
    return cascade == kCascadeCount - 1 ? lerp(seen, 1.0, cascade_band(frame, cascade, view_depth)) : seen;
}

// The fog between the camera and a point, as scene.fog works it out for a pixel: from the grid as far as it reaches
// (integrated: scene.fog_integrate's, with froxels), in closed form past it. Without the fog volumes' own share of a
// slice, so a surface inside one is a little off.
public void fog_in_front(SceneFrame* frame, uint integrated, uint grid_sampler, bool froxels, float3 position, float2 ndc,
                         out float3 transmittance, out float3 in_scatter)
{
    const float3 camera = frame.camera_position.xyz;
    const float3 to_point = position - camera;
    const float distance = length(to_point);
    const float3 direction = to_point / max(distance, 1e-4);
    float3 near_transmittance = float3(1.0);
    float3 near_scatter = float3(0.0);
    float from_distance = 0.0;
    const float reach = frame.fog.range.y;
    if (froxels && reach > 0.0)
    {
        const float view_per_ray = max(dot(direction, frame.camera_forward.xyz), 1e-3);
        const float slices = frame.fog.range.w;
        const float slice = froxel_t(frame.fog, min(distance * view_per_ray, reach)) * slices;
        const float2 grid_ndc = ndc - frame.taa_params.zw; // the grid doesn't move with TAA's offset
        const float3 uvw = float3(grid_ndc.x * 0.5 + 0.5, 0.5 - grid_ndc.y * 0.5, (slice - 0.5) / slices);
        const float4 grid = texture_3d(integrated).SampleLevel(sampler_state(grid_sampler), uvw, 0.0) * saturate(slice);
        near_transmittance = exp(-grid.a * max(frame.fog.extinction.rgb, float3(0.01)));
        near_scatter = grid.rgb;
        from_distance = reach / view_per_ray;
    }
    const FogResult far =
        fog_between(frame.fog, camera, direction, from_distance, distance, frame.sun_direction.xyz, frame.sun_color.rgb);
    transmittance = near_transmittance * far.transmittance;
    in_scatter = near_scatter + near_transmittance * far.in_scatter;
}
