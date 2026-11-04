// Froxel fog, step 1: per froxel of a view-aligned grid, the medium's extinction and the light it
// scatters toward the camera (common.fog), with the sun's shadows from the cascaded atlas. A few
// samples through the fog in the froxel, so a shadow edge inside it is a share of the light. Writes
// (light * e, e), e being the mean extinction * kFroxelDensityScale: light weighted by fog, so filtering
// across a froxel with none doesn't dim it. fog_integrate walks the columns.
// Over frames the samples move around inside the froxel, and the light blends into last frame's at the
// same place, so shafts come out smooth from few samples. Extinction is exact, so it takes no history.

import common.bindless;
import common.fog;
import scene.scene_common;

struct FogInjectPush
{
    SceneFrame* frame;
    uint volume; // RGBA16F 3D, storage: the froxels
    uint atlas;  // the sun's shadow atlas
    uint width;
    uint height;
    uint history;   // last frame's froxels; 0: none (the first frame, a cut, a gap, a new reach or size)
    uint sampler;   // linear clamp
    float jitter_x; // this frame's place for the samples, in [0, 1): across and down a column, along its depth
    float jitter_y;
    float jitter_z;
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
    if (last.a <= 0.0)
    {
        return false;
    }
    light = last.rgb / last.a;
    return true;
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
    const float extinction = mean_density * kFroxelDensityScale;
    if (extinction <= 0.0)
    {
        storage_3d(push.volume)[froxel] = float4(0.0);
        return;
    }

    // Each frame's own place in the froxel, a different one per froxel; in its middle with no history
    // to average that with.
    float3 place = float3(0.5);
    FroxelRay sample_ray = ray;
    if (push.history != 0)
    {
        const float2 at = float2(froxel.xy) + 5.588238 * float(froxel.z);
        const float3 noise = float3(gradient_noise(at), gradient_noise(at + 47.0), gradient_noise(at + 113.0));
        place = frac(noise + float3(push.jitter_x, push.jitter_y, push.jitter_z));
        sample_ray = froxel_ray(frame, float2(froxel.xy) + place.xy, grid);
    }

    // Its light: a few samples through the fog in its depth, weighted by the fog there, so a shadow edge
    // inside the slice is a share of the light.
    float weight_sum = 0.0;
    float3 light_sum = float3(0.0);
    for (uint i = 0; i < kSamples; ++i)
    {
        const float along = lerp(fog_near, fog_far, (float(i) + place.z) / float(kSamples));
        const float view_depth = along * ray.view_per_ray;
        const float distance = view_depth / sample_ray.view_per_ray;
        const float3 position = camera + sample_ray.direction * distance;
        const float height = position.y - frame.fog.density.w;
        const float weight = fog_density(frame.fog, height);
        float3 light = fog_ambient_light(frame.fog, height);
        if (distance >= frame.fog.sun_back.w)
        {
            light += fog_sun_light(frame.fog, height, sample_ray.direction, sun_direction, frame.sun_color.rgb) *
                     sun_visibility(frame, position, view_depth);
        }
        weight_sum += weight;
        light_sum += weight * light;
    }
    float3 light = weight_sum > 0.0 ? light_sum / weight_sum : float3(0.0);

    // Blended into last frame's at the same place: the middle of this froxel, as stored there.
    if (push.history != 0)
    {
        const float middle_depth = froxel_depth(frame.fog, (float(froxel.z) + 0.5) / float(slices));
        float3 last_light;
        if (history_light(frame, camera + ray.direction * (middle_depth / ray.view_per_ray), last_light))
        {
            light = lerp(light, last_light, kFeedback);
        }
    }
    if (any(isnan(light)))
    {
        light = float3(0.0); // one bad value lasts a frame: it mustn't live on in the history
    }
    storage_3d(push.volume)[froxel] = float4(min(light, kHalfMax) * extinction, extinction);
}
