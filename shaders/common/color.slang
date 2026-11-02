// Color helpers shared by shaders.

// A fully saturated color for a hue in [0, 1): red, yellow, green, cyan, blue, magenta, red.
public float3 hue_to_rgb(float hue)
{
    const float3 offsets = float3(0.0, 4.0, 2.0);
    return saturate(abs(fmod(hue * 6.0 + offsets, 6.0) - 3.0) - 1.0);
}

// sRGB-encoded values to linear (the exact curve, per channel).
public float3 srgb_to_linear(float3 srgb)
{
    const float3 low = srgb / 12.92;
    const float3 high = pow((srgb + 0.055) / 1.055, 2.4);
    return select(srgb <= 0.04045, low, high);
}
