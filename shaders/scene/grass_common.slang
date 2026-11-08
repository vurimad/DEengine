// What grass's passes share (docs/design.md, Terrain & vegetation: grass fields). Matches engine/render/scene/terrain.cpp.

// Matches asset::TerrainGrass.
public struct Grass
{
    public float density_per_m2;
    public float2 height_m;
    public float width_m;
    public float4 where_height_m;
    public float4 where_slope_cos;
    public float3 color_base;
    public uint excluded_masks;
    public float3 color_tip;
    public float patches;
    public float3 other_base;
    public uint other_mask; // 4: none
    public float3 other_tip;
    public uint reserved;
};

// A blade as the scatter writes it and the draw reads it.
public struct GrassBlade
{
    public float3 root;  // relative to the render origin
    public uint packed;  // yaw 8 bits, height 8, tone 8, lean 4, other color 4: each 0 to 1
};

// Blades of each level of detail lie in their own part of the buffer: nearest first, with the most triangles each.
public static const uint kGrassLods = 3;
public static const uint kGrassLodFirst[3] = {0u, 192u * 1024u, 448u * 1024u};
public static const uint kGrassLodCapacity[3] = {192u * 1024u, 256u * 1024u, 512u * 1024u};
public static const uint kGrassArgsWords = 5;
public static const uint kGrassArgsInstanceCount = 1;
// How far grass goes, in meters from the camera, and where each level of detail gives way to the next.
public static const float kGrassReachM = 220.0;
// Over this share of the reach blades thin out and shrink away, and the ground under them is grass-colored by then.
public static const float kGrassFadeFrom = 0.5;
public static const float kGrassLodEndM[2] = {18.0, 48.0};

// The share of the blades kept at a distance, 1 up close. Those left are wider, so the ground stays about as covered.
public float grass_kept(float distance_m)
{
    return 1.0 / (1.0 + pow(distance_m / 14.0, 1.6));
}

// How much grass grows here, 0 to 1, by the look's rules.
public float grass_where(Grass* grass, float height_m, float up, float4 masks)
{
    const float4 h = grass.where_height_m;
    float weight = saturate((height_m - h.x) / max(h.y - h.x, 1e-3)) * (1.0 - saturate((height_m - h.z) / max(h.w - h.z, 1e-3)));
    const float4 s = grass.where_slope_cos;
    weight *= saturate((s.x - up) / max(s.x - s.y, 1e-4)) * saturate((up - s.w) / max(s.z - s.w, 1e-4));
    const uint excluded = grass.excluded_masks;
    if (weight > 0.0 && excluded != 0u)
    {
        float most = 0.0;
        for (uint c = 0; c < 4; ++c)
        {
            if ((excluded & (1u << c)) != 0u)
            {
                most = max(most, masks[c]);
            }
        }
        weight *= 1.0 - smoothstep(0.25, 0.6, most);
    }
    return weight;
}

float grass_hash(float2 at)
{
    float3 p = frac(float3(at.xyx) * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

public float grass_noise(float2 at)
{
    const float2 cell = floor(at);
    float2 t = at - cell;
    t = t * t * (3.0 - 2.0 * t);
    return lerp(lerp(grass_hash(cell), grass_hash(cell + float2(1.0, 0.0)), t.x), lerp(grass_hash(cell + float2(0.0, 1.0)), grass_hash(cell + float2(1.0, 1.0)), t.x), t.y);
}

// The slow part of a blade's tone, which the ground under it shares, so the field stays one piece as blades give way to it.
public float grass_slow_tone(float2 world_xz)
{
    return grass_noise(world_xz / 41.0) * 0.45 + grass_noise(world_xz / 97.0) * 0.3 + grass_noise(world_xz / 310.0) * 0.25;
}
