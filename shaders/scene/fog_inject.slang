// Froxel fog, step 1: per froxel of a view-aligned grid, the medium's extinction and the light it
// scatters toward the camera (common.fog), with the sun's shadows from the cascaded atlas. A few
// samples through the fog in the froxel, so a shadow edge inside it is a share of the light. Writes
// the light weighted by the mean extinction (common.fog, kFroxelLightScale); fog_integrate walks the columns.
// Over frames the samples move around inside the froxel, and the light blends into last frame's at the
// same place, so shafts come out smooth from few samples. Extinction is exact, so it takes no history.
// Fog volumes add their extinction and tint here: lit and shadowed like the rest, at the grid's resolution.

import common.bindless;
import common.fog;
import scene.scene_common;

// A fog volume: world::RenderFogVolume, as the renderer writes it.
struct FogVolumeData
{
    float4 to_local[3];   // rows: into its own axes, in meters
    float4 half_extents;  // w: its soft edge, in meters
    float4 tint_density;  // w: extinction per meter at its heart
    float4 shape;         // x: 1 for an ellipsoid
};

struct FogInjectPush
{
    SceneFrame* frame;
    FogVolumeData* fog_volumes;
    uint volume; // RGBA16F 3D, storage: the froxels
    uint atlas;  // the sun's shadow atlas
    uint width;
    uint height;
    uint history;   // last frame's froxels; 0: none (the first frame, a cut, a gap, a new reach or size)
    uint sampler;   // linear clamp
    float jitter_x; // this frame's place for the samples, in [0, 1): across and down a column, along its depth
    float jitter_y;
    float jitter_z;
    uint fog_volume_count;
    uint padding;
};

[[vk::push_constant]]
ConstantBuffer<FogInjectPush> push;

static const uint kSamples = 4;
// How much of last frame's light a froxel keeps.
static const float kFeedback = 0.9;

// How much of the sun reaches a point at a view depth: the first cascade reaching that far; the last
// fades to lit over its band, as the surfaces' shadow mask does; past it, lit.
float sun_visibility(SceneFrame* frame, float3 position, float view_depth)
{
    const uint cascade = cascade_at(frame, view_depth);
    if (cascade == kCascadeCount)
    {
        return 1.0;
    }
    const float4 clip = cascade_clip(frame, cascade, position);
    const float2 tile_uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    const float seen = sample_shadow(push.atlas, atlas_uv(frame, cascade, tile_uv), clip.z);
    return cascade == kCascadeCount - 1 ? lerp(seen, 1.0, cascade_band(frame, cascade, view_depth)) : seen;
}

// How much of a volume's density a point has: 0 outside, to 1 an edge in from its surface.
float fog_volume_share(FogVolumeData volume, float3 position)
{
    const float3 local = float3(dot(volume.to_local[0], float4(position, 1.0)), dot(volume.to_local[1], float4(position, 1.0)),
                                dot(volume.to_local[2], float4(position, 1.0)));
    const float3 half_extents = max(volume.half_extents.xyz, float3(1e-3));
    float inside;
    if (volume.shape.x > 0.5)
    {
        // Approximately, in meters: right along its smallest axis, and an underestimate elsewhere.
        inside = (1.0 - length(local / half_extents)) * min(half_extents.x, min(half_extents.y, half_extents.z));
    }
    else
    {
        const float3 from_faces = half_extents - abs(local);
        inside = min(from_faces.x, min(from_faces.y, from_faces.z));
    }
    return smoothstep(0.0, 1.0, saturate(inside / max(volume.half_extents.w, 1e-3)));
}

// Last frame's light at a point, if it was in the grid and in fog then.
bool history_light(SceneFrame* frame, float3 position, out float3 light)
{
    light = float3(0.0);
    const float4 clip = mul_rows(frame.previous_view_proj, float4(position, 1.0));
    if (clip.w <= 1e-4)
    {
        return false;
    }
    const float3 uvw = float3(clip.x / clip.w * 0.5 + 0.5, 0.5 - clip.y / clip.w * 0.5, froxel_t(frame.fog, clip.w));
    if (any(uvw < 0.0) || any(uvw > 1.0))
    {
        return false;
    }
    const float4 last = texture_3d(push.history).SampleLevel(sampler_state(push.sampler), uvw, 0.0);
    if (!(last.a > 0.0))
    {
        return false;
    }
    light = last.rgb * kFroxelLightScale / last.a;
    return all(isfinite(light)); // a bad value lasts its frame
}

[shader("compute")]
[numthreads(4, 4, 4)]
void compute_main(uint3 froxel: SV_DispatchThreadID)
{
    SceneFrame* frame = push.frame;
    const uint slices = uint(frame.fog.range.w);
    if (froxel.x >= push.width || froxel.y >= push.height || froxel.z >= slices)
    {
        return;
    }
    const uint2 grid = uint2(push.width, push.height);
    const FroxelRay ray = froxel_ray(frame, float2(froxel.xy) + 0.5, grid);
    const float3 camera = frame.camera_position.xyz;
    const float3 sun_direction = frame.sun_direction.xyz;
    const float near_depth = froxel_depth(frame.fog, float(froxel.z) / float(slices));
    const float far_depth = froxel_depth(frame.fog, float(froxel.z + 1) / float(slices));

    // The slice's extinction, exactly: the medium's optical depth across it, within where fog starts
    // and ends, as a mean over its whole span.
    const float near_distance = near_depth / ray.view_per_ray;
    const float far_distance = far_depth / ray.view_per_ray;
    const float fog_near = clamp(frame.fog.ambient.w, near_distance, far_distance);
    const float fog_far = clamp(frame.fog.range.x, near_distance, far_distance);
    const float mean_density = fog_far > fog_near ? optical_depth(frame.fog, camera.y, ray.direction.y, fog_near, fog_far) /
                                                        (far_distance - near_distance)
                                                  : 0.0;

    // The fog volumes around its middle, once, not per sample: extinction stays exact from frame to frame,
    // and a soft edge doesn't change much across a froxel. They fade out at the grid's reach, where the
    // fog past it, which can't hold them, takes over.
    float volume_density = 0.0;
    float3 volume_tinted = float3(0.0);
    const float middle_depth = froxel_depth(frame.fog, (float(froxel.z) + 0.5) / float(slices));
    if (push.fog_volume_count > 0)
    {
        const float3 middle = camera + ray.direction * (middle_depth / ray.view_per_ray);
        const float reach = frame.fog.range.y;
        const float reach_fade = saturate((reach - middle_depth) / (reach * 0.1));
        for (uint i = 0; i < push.fog_volume_count; ++i)
        {
            const FogVolumeData volume = push.fog_volumes[i];
            const float density = volume.tint_density.w * fog_volume_share(volume, middle) * reach_fade;
            volume_density += density;
            volume_tinted += density * volume.tint_density.rgb;
        }
    }
    const float all_density = mean_density + volume_density;
    const float extinction = all_density * kFroxelDensityScale;
    if (!(extinction > 0.0))
    {
        storage_3d(push.volume)[froxel] = float4(0.0);
        return;
    }

    // Last frame's light at the same place: the middle of this froxel, as stored there.
    float3 last_light = float3(0.0);
    bool has_history = false;
    if (push.history != 0)
    {
        has_history = history_light(frame, camera + ray.direction * (middle_depth / ray.view_per_ray), last_light);
    }

    // Each frame's own place in the froxel, a different one per froxel; in its middle with no history
    // to average that with.
    float3 place = float3(0.5);
    FroxelRay sample_ray = ray;
    if (has_history)
    {
        const float2 at = float2(froxel.xy) + 5.588238 * float(froxel.z);
        const float3 noise = float3(gradient_noise(at), gradient_noise(at.yx), gradient_noise(at * 1.618));
        place = frac(noise + float3(push.jitter_x, push.jitter_y, push.jitter_z));
        sample_ray = froxel_ray(frame, float2(froxel.xy) + place.xy, grid);
    }

    // Its light: a few samples through the fog in its depth, weighted by the fog there, so a shadow edge
    // inside the slice is a share of the light.
    const float samples_from = mean_density > 0.0 ? fog_near : near_distance;
    const float samples_to = mean_density > 0.0 ? fog_far : far_distance;
    float weight_sum = 0.0;
    float3 light_sum = float3(0.0);
    for (uint i = 0; i < kSamples; ++i)
    {
        const float distance = lerp(samples_from, samples_to, (float(i) + place.z) / float(kSamples));
        const float view_depth = distance * sample_ray.view_per_ray;
        const float3 position = camera + sample_ray.direction * distance;
        const float height = position.y - frame.fog.density.w;
        const float weight = fog_density(frame.fog, height) + volume_density;
        float3 light = fog_ambient_light(frame.fog, height);
        if (distance >= frame.fog.sun_back.w)
        {
            light += fog_sun_light(frame.fog, height, sample_ray.direction, sun_direction, frame.sun_color.rgb) *
                     sun_visibility(frame, position, view_depth);
        }
        weight_sum += weight;
        light_sum += weight * light;
    }
    // A volume's tint on its share of the light.
    const float3 tint = (float3(mean_density) + volume_tinted) / all_density;
    float3 light = weight_sum > 0.0 ? light_sum / weight_sum * tint : float3(0.0);

    // A bad value lasts its frame: it mustn't reach the history, or take what's there with it.
    if (!all(isfinite(light)))
    {
        light = last_light;
    }
    if (has_history)
    {
        light = lerp(light, last_light, kFeedback);
    }
    // What's stored is the product, so it's the product that has to fit a half float: over it would be
    // infinite from then on.
    const float weight = extinction / kFroxelLightScale;
    storage_3d(push.volume)[froxel] = float4(min(light, kHalfMax / max(weight, 1.0)) * weight, extinction);
}
