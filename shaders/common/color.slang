// Color helpers shared by shaders.

// A fully saturated color for a hue in [0, 1): red, yellow, green, cyan, blue, magenta, red.
public float3 hue_to_rgb(float hue)
{
    const float3 offsets = float3(0.0, 4.0, 2.0);
    return saturate(abs(fmod(hue * 6.0 + offsets, 6.0) - 3.0) - 1.0);
}
