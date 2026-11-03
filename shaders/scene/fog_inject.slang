// Froxel fog, step 1: per froxel of a view-aligned grid, the medium's extinction and the light it
// scatters toward the camera (common.fog), with the sun's shadows from the cascaded atlas. A few
// samples through the froxel's depth, so a shadow edge inside it is a share of the light. Writes
// (light per unit of extinction, extinction); fog_integrate walks the columns.

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
};

[[vk::push_constant]]
ConstantBuffer<FogInjectPush> push;

static const uint kSamples = 4;

// How much of the sun reaches a point at a view depth: the first cascade reaching that far; past the
// last, lit.
float sun_visibility(SceneFrame* frame, float3 position, float view_depth)
{
    for (uint cascade = 0; cascade < kCascadeCount; ++cascade)
    {
        if (view_depth < frame.cascade_far[cascade])
        {
            const float4 clip = cascade_clip(frame, cascade, position);
            const float2 tile_uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
            const float border = frame.shadow_params.y * 2.0;
            const float2 uv = cascade_tile(cascade) + clamp(tile_uv, border, 1.0 - border) * 0.5;
            return sample_shadow(push.atlas, uv, clip.z);
        }
    }
    return 1.0;
}

[shader("compute")]
[numthreads(4, 4, 4)]
void compute_main(uint3 froxel: SV_DispatchThreadID)
{
    SceneFrame* frame = push.frame;
    const float slices = frame.fog.range.w;
    if (froxel.x >= push.width || froxel.y >= push.height || float(froxel.z) >= slices)
    {
        return;
    }
    const float2 ndc = float2((float(froxel.x) + 0.5) / float(push.width) * 2.0 - 1.0,
                              1.0 - (float(froxel.y) + 0.5) / float(push.height) * 2.0);
    const float3 camera = frame.camera_position.xyz;
    const float3 direction = normalize(position_from_depth(frame, ndc, 1.0) - camera);
    const float view_per_ray = max(dot(direction, frame.camera_forward.xyz), 1e-3); // view depth per meter along the ray
    const float3 sun_direction = frame.sun_direction.xyz;

    const float near_depth = froxel_depth(frame.fog, float(froxel.z) / slices);
    const float far_depth = froxel_depth(frame.fog, float(froxel.z + 1) / slices);
    float density_sum = 0.0;
    float3 light_sum = float3(0.0);
    for (uint i = 0; i < kSamples; ++i)
    {
        const float view_depth = lerp(near_depth, far_depth, (float(i) + 0.5) / float(kSamples));
        const float distance = view_depth / view_per_ray;
        if (distance < frame.fog.ambient.w || distance > frame.fog.range.x)
        {
            continue; // before fog starts, or past where it ends
        }
        const float3 position = camera + direction * distance;
        const float height = position.y - frame.fog.density.w;
        const float density = fog_density(frame.fog, height);
        float3 light = fog_ambient_light(frame.fog, height);
        if (distance >= frame.fog.sun_back.w)
        {
            light += fog_sun_light(frame.fog, height, direction, sun_direction, frame.sun_color.rgb) *
                     sun_visibility(frame, position, view_depth);
        }
        density_sum += density;
        light_sum += density * light;
    }
    const float3 light = density_sum > 0.0 ? light_sum / density_sum : float3(0.0);
    storage_3d(push.volume)[froxel] = float4(min(light, kHalfMax), density_sum / float(kSamples));
}
