// Froxel fog, step 2: each column of froxels, walked front to back. Each slice's fog is integrated in
// closed form (its light and extinction taken as constant across it), so a slice's thickness doesn't
// change how much light it adds. Writes, per slice: the in-scatter from the camera through that slice,
// and the transmittance before the per-channel scale (a channel's is that to the power of its scale).

import common.bindless;
import common.fog;
import scene.scene_common;

struct FogIntegratePush
{
    SceneFrame* frame;
    uint froxels;    // RGBA16F 3D, sampled: (light per unit of extinction, extinction)
    uint integrated; // RGBA16F 3D, storage: (in-scatter, transmittance)
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
    const float2 ndc = float2((float(column.x) + 0.5) / float(push.width) * 2.0 - 1.0,
                              1.0 - (float(column.y) + 0.5) / float(push.height) * 2.0);
    const float3 direction = normalize(position_from_depth(frame, ndc, 1.0) - frame.camera_position.xyz);
    const float view_per_ray = max(dot(direction, frame.camera_forward.xyz), 1e-3);
    const float3 extinction = max(frame.fog.extinction.rgb, float3(0.01));

    Texture3D froxels = texture_3d(push.froxels);
    RWTexture3D<float4> integrated = storage_3d(push.integrated);
    float3 in_scatter = float3(0.0);
    float transmittance = 1.0;
    float near_depth = 0.0;
    for (uint slice = 0; slice < slices; ++slice)
    {
        const float far_depth = froxel_depth(frame.fog, float(slice + 1) / float(slices));
        const float span = (far_depth - near_depth) / view_per_ray;
        const float4 froxel = froxels.Load(int4(column.xy, slice, 0));
        const float depth = froxel.a * span;
        in_scatter += froxel.rgb * (1.0 - exp(-depth * extinction)) / extinction * pow(transmittance, extinction);
        transmittance *= exp(-depth);
        integrated[uint3(column.xy, slice)] = float4(min(in_scatter, kHalfMax), transmittance);
        near_depth = far_depth;
    }
}
