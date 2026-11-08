// Light baked over the terrain for the environment's sun (its .terrainlayers "lighting" map; tools/worldgen writes it):
// how deep in shadow each place lies, and how much sky it sees. It's what gives a far cliff its shadow across a valley,
// past the reach of the cascades, and darkens valleys and the feet of cliffs. White is all lit, so a map still loading
// changes nothing.
//   r: 1 - sqrt(depth / 1024 m), the depth being how far above the ground the sun is still hidden
//   g: the share of the sky seen

import common.bindless;
import scene.scene_common;

public struct TerrainLight
{
    public float shadow_depth_m; // 0: in the sun
    public float sky;            // 0 to 1
};

// At a point in render-origin space. Off the terrain, or without a map: all lit.
public TerrainLight terrain_light(SceneFrame* frame, float3 position)
{
    TerrainLight light;
    light.shadow_depth_m = 0.0;
    light.sky = 1.0;
    if (frame.terrain_light.y == 0u)
    {
        return light;
    }
    const float2 from_corner = position.xz + float2(int2(frame.render_origin_steps.xz - frame.terrain_origin_steps)) * kMetersPerRenderStep;
    const float2 uv = from_corner / (float2(frame.terrain_size_steps) * kMetersPerRenderStep);
    if (any(uv < 0.0) || any(uv > 1.0))
    {
        return light;
    }
    const uint texture = frame.materials[frame.terrain_light.x].metallic_roughness_texture;
    const float2 rg = texture_2d(texture).SampleLevel(sampler_state(kSamplerLinearClamp), uv, 0.0).rg;
    const float hidden = 1.0 - rg.r;
    light.shadow_depth_m = hidden * hidden * 1024.0;
    light.sky = rg.g;
    return light;
}
