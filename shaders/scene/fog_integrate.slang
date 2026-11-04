// Froxel fog, step 2: each column of froxels, walked front to back. Each slice's fog is integrated in
// closed form (its light and extinction taken as constant across it), so a slice's thickness doesn't
// change how much light it adds. Writes, per slice: the in-scatter from the camera through that slice,
// and the optical depth before the per-channel scale. Depth, not transmittance: it interpolates
// correctly between slices, and a channel with a small scale doesn't hit a floor when it's read.

import common.bindless;
import common.fog;
import scene.scene_common;

struct FogIntegratePush
{
    SceneFrame* frame;
    uint froxels;    // RGBA16F 3D, sampled: (light per unit of extinction * e, e), e = extinction * kFroxelDensityScale
    uint integrated; // RGBA16F 3D, storage: (in-scatter, optical depth)
    uint width;
    uint height;
};

[[vk::push_constant]]
ConstantBuffer<FogIntegratePush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 column: SV_DispatchThreadID)
{
    if (column.x >= push.width || column.y >= push.height)
    {
        return;
    }
    SceneFrame* frame = push.frame;
    const uint slices = uint(frame.fog.range.w);
    const FroxelRay ray = froxel_ray(frame, float2(column.xy) + 0.5, uint2(push.width, push.height));
    const float3 extinction = max(frame.fog.extinction.rgb, float3(0.01));

    Texture3D froxels = texture_3d(push.froxels);
    RWTexture3D<float4> integrated = storage_3d(push.integrated);
    float3 in_scatter = float3(0.0);
    float optical = 0.0;
    float near_depth = 0.0;
    for (uint slice = 0; slice < slices; ++slice)
    {
        const float far_depth = froxel_depth(frame.fog, float(slice + 1) / float(slices));
        const float span = (far_depth - near_depth) / ray.view_per_ray;
        const float4 froxel = froxels.Load(int4(column.xy, slice, 0));
        const float depth = froxel.a / kFroxelDensityScale * span;
        const float3 light = froxel.a > 0.0 ? froxel.rgb / froxel.a : float3(0.0);
        in_scatter += light * (1.0 - exp(-depth * extinction)) / extinction * exp(-optical * extinction);
        optical += depth;
        integrated[uint3(column.xy, slice)] = float4(min(in_scatter, kHalfMax), min(optical, kHalfMax));
        near_depth = far_depth;
    }
}
