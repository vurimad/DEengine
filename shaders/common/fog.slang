// Fog: one medium model for the whole view (docs/design.md, Sky, atmosphere & weather). Extinction
// is a constant everywhere plus a height fog whose density follows a bounded exponential profile
// around a base height: thinning upward, and below it rising only to twice its base density, so
// valleys don't fill without limit. Light scattered into the view: the sun, through a forward and a back
// colour (looking toward it or away from it) and dimmed by the fog column above, plus an ambient
// colour. Extinction can be coloured per channel, so blue fades first and the sun reddens through
// haze. Along a ray the height fog has a closed form, so that is exact for the medium as modelled, at
// any distance. Near the camera, a froxel grid (scene/fog_inject, scene/fog_integrate) takes the same
// medium and adds what no closed form can: the sun's shadows in the fog. Past the grid, the ray goes
// on in closed form.

// The medium and its light, in render-origin space (the CPU's render::SceneFog, packed).
public struct FogMedium
{
    public float4 density;     // x: constant extinction (1/m), y: height fog's at its base (1/m),
                               // z: height falloff (1/m), w: the base height
    public float4 extinction;  // rgb: per-channel extinction scale, w: ambient's height attenuation
    public float4 sun_forward; // rgb: in-scatter looking toward the sun (times its colour), w: the
                               // sun's height attenuation
    public float4 sun_back;    // rgb: looking away from it, w: where sun haze starts (m)
    public float4 ambient;     // rgb: isotropic in-scatter, w: where fog starts (m)
    public float4 range;       // x: where it ends (m): the sky's distance, y: how far the froxel grid
                               // reaches (m, view depth; 0: no grid), z: its slice offset (m), w: its slices
};

// What a ray through the fog leaves of the light behind it, and adds: colour * transmittance + in_scatter.
public struct FogResult
{
    public float3 transmittance;
    public float3 in_scatter;
};

// The height profile in units of the falloff: 1 at the base, e^-u above, 2 - e^u below.
float height_profile(float u)
{
    return u >= 0.0 ? exp(-u) : 2.0 - exp(u);
}

// The profile's mean over [u0, u1], in closed form. Each side of 0 is integrated on its own, so no
// two large values are subtracted (far below the base, 2u - e^u would lose every digit).
float mean_profile(float u0, float u1)
{
    const float a = min(u0, u1);
    const float b = max(u0, u1);
    if (b - a < 1e-4)
    {
        return height_profile(0.5 * (a + b));
    }
    const float below = a < 0.0 ? 2.0 * (min(b, 0.0) - a) - (exp(min(b, 0.0)) - exp(a)) : 0.0;
    const float above = b > 0.0 ? exp(-max(a, 0.0)) - exp(-b) : 0.0;
    return (below + above) / (b - a);
}

// The height fog above height h (relative to the base), per unit of base extinction, in meters:
// the column the sun and the sky shine down through.
float column_above(float h, float falloff)
{
    const float u = falloff * h;
    return (u >= 0.0 ? exp(-u) : exp(u) - 2.0 * u) / falloff;
}

// Optical depth (before the per-channel scale) along the ray from distance t0 to t1.
public float optical_depth(FogMedium medium, float camera_height, float direction_y, float t0, float t1)
{
    const float falloff = max(medium.density.z, 1e-6);
    const float u0 = falloff * (camera_height + direction_y * t0 - medium.density.w);
    const float u1 = falloff * (camera_height + direction_y * t1 - medium.density.w);
    return (medium.density.x + medium.density.y * mean_profile(u0, u1)) * (t1 - t0);
}

// Extinction at a height relative to the base, before the per-channel scale (1/m).
public float fog_density(FogMedium medium, float height)
{
    return medium.density.x + medium.density.y * height_profile(max(medium.density.z, 1e-6) * height);
}

// The light one unit of extinction scatters toward the camera at a height relative to the base,
// looking along direction: the sun's (through the fog column above, by its forward and back colours)
// and the ambient's (through the same column).
public float3 fog_sun_light(FogMedium medium, float height, float3 direction, float3 sun_direction, float3 sun_color)
{
    const float3 extinction = max(medium.extinction.rgb, float3(0.01));
    const float column = medium.density.y * column_above(height, max(medium.density.z, 1e-6));
    const float mu = dot(direction, sun_direction);
    const float3 phase = lerp(medium.sun_back.rgb, medium.sun_forward.rgb, mu * 0.5 + 0.5);
    return sun_color * phase * exp(-extinction * column * medium.sun_forward.w / max(sun_direction.y, 0.05));
}

public float3 fog_ambient_light(FogMedium medium, float height)
{
    const float3 extinction = max(medium.extinction.rgb, float3(0.01));
    const float column = medium.density.y * column_above(height, max(medium.density.z, 1e-6));
    return medium.ambient.rgb * exp(-extinction * column * medium.extinction.w);
}

// The grid stores extinction (1/m) times this: thin fog stays a normal half float (a subnormal may
// flush to zero on some GPUs). What it must hold: 3 per meter of world fog (FogLimits: 1 constant, and 1 of
// height fog that doubles below its base), and fog_inject's kMostVolumeDensity of 12 from volumes. That's
// 61440 of a half float's 65504, so raising either needs a smaller scale.
public static const float kFroxelDensityScale = 4096.0;
// A froxel stores (light * e / kFroxelLightScale, e), e being extinction * kFroxelDensityScale: light
// weighted by fog, so filtering across a froxel with none doesn't dim it, with room in a half float for
// bright light in dense fog.
public static const float kFroxelLightScale = 64.0;

// The froxel grid's slices: view depth from 0 to its reach, spaced exponentially with an offset (no
// slices vanishingly thin at the camera). t runs 0 to 1 across the grid.
public float froxel_depth(FogMedium medium, float t)
{
    const float offset = medium.range.z;
    return offset * (pow(medium.range.y / offset + 1.0, t) - 1.0);
}

public float froxel_t(FogMedium medium, float depth)
{
    const float offset = medium.range.z;
    return log(depth / offset + 1.0) / log(medium.range.y / offset + 1.0);
}

// A fog volume: world::RenderFogVolume, as the renderer writes it.
public struct FogVolumeData
{
    public float4 to_local[3];  // rows: into its own axes, in meters
    public float4 half_extents; // w: its soft edge, in meters
    public float4 tint_density; // w: extinction per meter at its heart
    public float4 shape;        // x: 1 for an ellipsoid
};

// Points tested through the part of a ray inside a volume.
static const uint kFogVolumeSamples = 3;

// How much of a volume's density a point in its own axes has: 0 outside, to 1 an edge in from its
// surface. An edge wider than the volume ends at its middle, so its heart is always at full density.
float fog_volume_share(FogVolumeData volume, float3 local)
{
    const float3 half_extents = max(volume.half_extents.xyz, float3(1e-3));
    const float smallest = min(half_extents.x, min(half_extents.y, half_extents.z));
    float inside;
    if (volume.shape.x > 0.5)
    {
        // Approximately, in meters: right along its smallest axis, and an underestimate elsewhere.
        inside = (1.0 - length(local / half_extents)) * smallest;
    }
    else
    {
        const float3 from_faces = half_extents - abs(local);
        inside = min(from_faces.x, min(from_faces.y, from_faces.z));
    }
    return smoothstep(0.0, 1.0, saturate(inside / max(min(volume.half_extents.w, smallest), 1e-3)));
}

// A volume's optical depth along a ray between two distances. The ray is clipped to the volume first and
// tested through what's left, so the points belong to the volume, not to what asked: however thin it is
// along the ray they can't all miss it, and they don't slide through it as the grid's slices do with the
// camera. It fades out over the last tenth of the grid's reach, where the fog past it, which can't hold it,
// takes over. Widens span to what of the ray it covers.
float fog_volume_optical_depth(FogVolumeData volume, float3 camera, float3 direction, float view_per_ray, float reach,
                               float from_distance, float to_distance, inout float2 span)
{
    const float3 origin = float3(dot(volume.to_local[0], float4(camera, 1.0)), dot(volume.to_local[1], float4(camera, 1.0)),
                                 dot(volume.to_local[2], float4(camera, 1.0)));
    const float3 along = float3(dot(volume.to_local[0].xyz, direction), dot(volume.to_local[1].xyz, direction),
                                dot(volume.to_local[2].xyz, direction));
    const float3 half_extents = max(volume.half_extents.xyz, float3(1e-3));
    float enter = from_distance;
    float leave = to_distance;
    if (volume.shape.x > 0.5)
    {
        const float3 scaled_origin = origin / half_extents;
        const float3 scaled_along = along / half_extents;
        const float a = dot(scaled_along, scaled_along);
        const float b = dot(scaled_origin, scaled_along);
        const float root = b * b - a * (dot(scaled_origin, scaled_origin) - 1.0);
        if (!(root > 0.0))
        {
            return 0.0;
        }
        enter = max(enter, (-b - sqrt(root)) / a);
        leave = min(leave, (-b + sqrt(root)) / a);
    }
    else
    {
        // Never quite along a face, so a ray in its plane gets infinities with the right signs, not a NaN.
        const float3 safe_along = select(abs(along) > 1e-6, along, float3(1e-6));
        const float3 first = (-half_extents - origin) / safe_along;
        const float3 second = (half_extents - origin) / safe_along;
        const float3 nearer = min(first, second);
        const float3 farther = max(first, second);
        enter = max(enter, max(nearer.x, max(nearer.y, nearer.z)));
        leave = min(leave, min(farther.x, min(farther.y, farther.z)));
    }
    if (!(leave > enter))
    {
        return 0.0;
    }
    span = float2(min(span.x, enter), max(span.y, leave));
    float sum = 0.0;
    for (uint i = 0; i < kFogVolumeSamples; ++i)
    {
        const float distance = lerp(enter, leave, (float(i) + 0.5) / float(kFogVolumeSamples));
        const float reach_fade = saturate((reach - distance * view_per_ray) / (reach * 0.1));
        sum += fog_volume_share(volume, origin + along * distance) * reach_fade;
    }
    return volume.tint_density.w * sum * (leave - enter) / float(kFogVolumeSamples);
}

// All the volumes in a slice's mask, along a ray between two distances: (their tint by optical depth,
// their optical depth), and the span of the ray they cover: empty (x > y) if none.
public float4 fog_volumes_along(FogVolumeData* volumes, uint2 mask, float3 camera, float3 direction, float view_per_ray,
                                float reach, float from_distance, float to_distance, out float2 span)
{
    span = float2(1e30, -1e30);
    float4 total = float4(0.0);
    for (uint word = 0; word < 2; ++word)
    {
        for (uint bits = mask[word]; bits != 0; bits &= bits - 1)
        {
            const FogVolumeData volume = volumes[word * 32 + firstbitlow(bits)];
            const float depth = fog_volume_optical_depth(volume, camera, direction, view_per_ray, reach, from_distance, to_distance, span);
            total += float4(volume.tint_density.rgb * depth, depth);
        }
    }
    return total;
}

// The fog along a ray (direction unit, sun_direction unit and toward the sun) from from_distance to
// distance meters, within where fog starts and ends.
public FogResult fog_between(FogMedium medium, float3 camera, float3 direction, float from_distance, float distance,
                             float3 sun_direction, float3 sun_color)
{
    FogResult result;
    result.transmittance = float3(1.0);
    result.in_scatter = float3(0.0);
    const float near = max(medium.ambient.w, from_distance);
    const float far = min(distance, medium.range.x);
    if (far <= near)
    {
        return result;
    }
    const float3 extinction = max(medium.extinction.rgb, float3(0.01));
    const float depth = optical_depth(medium, camera.y, direction.y, near, far);
    result.transmittance = exp(-depth * extinction);

    // Where the light reaching the camera was scattered in: the mean distance weighted by what gets
    // through (for a uniform medium, 1/sigma - span T / (1 - T); half way when the fog is thin). The
    // sun and sky light the fog as they do there: through the column above that point.
    const float span = far - near;
    const float transmitted = exp(-depth);
    const float mean_t = depth < 1e-3 ? 0.5 * span : clamp(span / depth - span * transmitted / (1.0 - transmitted), 0.0, span);
    const float height = camera.y + direction.y * (near + mean_t) - medium.density.w;
    const float3 sun = fog_sun_light(medium, height, direction, sun_direction, sun_color);
    const float3 ambient = fog_ambient_light(medium, height);

    // Scattering is extinction before the per-channel scale: what a channel loses to it, it gains of
    // the light in that proportion. Sun haze only from where it starts: the in-scatter past it is
    // what the fog in front of it lets through, less what's left at the far end.
    const float sun_start = clamp(medium.sun_back.w, near, far);
    const float3 through_sun_start = exp(-optical_depth(medium, camera.y, direction.y, near, sun_start) * extinction);
    result.in_scatter = ((through_sun_start - result.transmittance) * sun + (1.0 - result.transmittance) * ambient) / extinction;
    return result;
}
