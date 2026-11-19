// What surfaces share for reflecting light: GGX, height-correlated Smith visibility, Schlick's Fresnel, and the
// split-sum's environment BRDF, fitted.

public static const float kPi = 3.14159265;

public float d_ggx(float n_dot_h, float a)
{
    const float a2 = a * a;
    const float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

// Height-correlated Smith visibility, approximated (the 4 n.l n.v of the BRDF folded in).
public float v_smith(float n_dot_v, float n_dot_l, float a)
{
    return 0.5 / max(lerp(2.0 * n_dot_l * n_dot_v, n_dot_l + n_dot_v, a), 1e-5);
}

public float3 f_schlick(float3 f0, float v_dot_h)
{
    return f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
}

// The split-sum's environment BRDF, fitted (no lookup table): scale and bias on F0.
public float3 env_brdf(float3 f0, float roughness, float n_dot_v)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

// How much of a reflection's light an occluded spot still gets: more of it the smoother the surface and the more
// it's seen face on (Lagarde and de Rousiers, "Moving Frostbite to PBR", 2014).
public float specular_occlusion(float n_dot_v, float visibility, float roughness)
{
    return saturate(pow(n_dot_v + visibility, exp2(-16.0 * roughness - 1.0)) - 1.0 + visibility);
}
