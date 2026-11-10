// The editor's selection mask when the terrain is chosen: every pixel the terrain or its grass drew (its ID,
// kEditorTerrainId), and what stands on it, so the outline goes round the ground's edge and skyline, not round each
// rock on it. As the meshes' mask: x covered, y seen. A compute pass: the IDs are read as a storage texture.

import common.bindless;
import scene.scene_common;

struct TerrainSelectionPush
{
    uint ids;  // the G-buffer's instance IDs (R32 uint, storage)
    uint mask; // RGBA8, storage
    uint width;
    uint height;
};

[[vk::push_constant]]
ConstantBuffer<TerrainSelectionPush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 pixel: SV_DispatchThreadID)
{
    if (pixel.x >= push.width || pixel.y >= push.height)
    {
        return;
    }
    const uint id = storage_2d_uint(push.ids)[pixel.xy];
    const float covered = id != 0u ? 1.0 : 0.0; // the sky has none
    storage_2d(push.mask)[pixel.xy] = float4(covered, covered, 0.0, 0.0);
}
