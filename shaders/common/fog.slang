// Fog: one medium model for the whole view (docs/design.md, Sky, atmosphere & weather). Extinction
// is a constant everywhere plus a height fog whose density follows a bounded exponential profile
// around a base height: thinning upward, and below it rising only to twice its base density, so
// valleys don't fill without limit. Light scattered into the view: the sun, through a forward and a back
// colour (looking toward it or away from it) and dimmed by the fog column above, plus an ambient
// colour. Extinction can be coloured per channel, so blue fades first and the sun reddens through
// haze. Along a ray the height fog has a closed form, so this is exact for the medium as modelled,
// at any distance: the far range of the froxel fog to come, and all of the fog until then.

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
    public float4 range;       // x: where it ends (m): the sky's distance
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

// Its antiderivative, continuous at 0.
float height_profile_integral(float u)
{
    return u >= 0.0 ? -exp(-u) : 2.0 * u - exp(u);
}

// The height fog above height h (relative to the base), per unit of base extinction, in meters:
// the column the sun and the sky shine down through.
float column_above(float h, float falloff)
{
    return -height_profile_integral(falloff * h) / falloff;
}

// The fog between camera and a point distance meters along direction (unit), from start to end.
// sun_direction (unit) points toward the sun.
public FogResult fog_along_ray(FogMedium medium, float3 camera, float3 direction, float distance,
                               float3 sun_direction, float3 sun_color)
{
    FogResult result;
    result.transmittance = float3(1.0);
    result.in_scatter = float3(0.0);
    const float near = medium.ambient.w;
    const float far = min(distance, medium.range.x);
    if (far <= near)
    {
        return result;
    }
    const float span = far - near;
    const float falloff = max(medium.density.z, 1e-6);
    const float h0 = camera.y + direction.y * near - medium.density.w;
    const float h1 = camera.y + direction.y * far - medium.density.w;

    // Optical depth: the constant part, and the height fog's integral along the ray (closed form;
    // a level ray is constant density).
    const float du = falloff * (h1 - h0);
    const float height_path = abs(du) < 1e-4
                                  ? height_profile(falloff * h0) * span
                                  : span * (height_profile_integral(falloff * h1) - height_profile_integral(falloff * h0)) / du;
    const float depth = medium.density.x * span + medium.density.y * height_path;
    const float3 extinction = max(medium.extinction.rgb, float3(0.01));
    result.transmittance = exp(-depth * extinction);

    // The light scattered in, taken as the light at the segment's middle height.
    const float column = medium.density.y * column_above(0.5 * (h0 + h1), falloff);
    const float mu = dot(direction, sun_direction);
    const float3 phase = lerp(medium.sun_back.rgb, medium.sun_forward.rgb, mu * 0.5 + 0.5);
    const float3 sun_through = exp(-extinction * column * medium.sun_forward.w / max(sun_direction.y, 0.05));
    const float sun_share = saturate((far - max(near, medium.sun_back.w)) / span); // past sun haze's start
    const float3 sun = sun_color * phase * sun_through * sun_share;
    const float3 ambient = medium.ambient.rgb * exp(-medium.extinction.w * column);
    // Scattering is extinction before the per-channel scale: what a channel loses to it, it gains of
    // the light in that proportion.
    result.in_scatter = (1.0 - result.transmittance) * (sun + ambient) / extinction;
    return result;
}
