// Deferred lighting (renderer step 2): one full-screen pass over the G-buffer, added onto the HDR
// target (which holds the emissive light). The sun: GGX specular (height-correlated Smith,
// Schlick Fresnel) and Lambert diffuse, wrapped and with backlight transmission for translucent
// surfaces. Ambient: a sky/ground hemisphere, the placeholder for image-based lighting. Where
// nothing was drawn (depth at the far plane), the sky.

import common.bindless;
import scene.scene_common;

struct DeferredLightPush
{
    SceneFrame* frame;
    uint gb0;
    uint gb1;
    uint gb2;
    uint depth;
    uint shadow_mask; // r: sunlight seen
};

[[vk::push_constant]]
ConstantBuffer<DeferredLightPush> push;

static const float kPi = 3.14159265;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 ndc : TEXCOORD;
};

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VertexID)
{
    VertexOutput output;
    output.position = fullscreen_triangle(vertex_id, output.ndc);
    return output;
}

float d_ggx(float n_dot_h, float a)
{
    const float a2 = a * a;
    const float d = n_dot_h * n_dot_h * (a2 - 1.0) + 1.0;
    return a2 / (kPi * d * d);
}

// Height-correlated Smith visibility, approximated (the 4 n.l n.v of the BRDF folded in).
float v_smith(float n_dot_v, float n_dot_l, float a)
{
    return 0.5 / max(lerp(2.0 * n_dot_l * n_dot_v, n_dot_l + n_dot_v, a), 1e-5);
}

float3 f_schlick(float3 f0, float v_dot_h)
{
    return f0 + (1.0 - f0) * pow(1.0 - v_dot_h, 5.0);
}

// The split-sum's environment BRDF, fitted (no lookup table): scale and bias on F0.
float3 env_brdf(float3 f0, float roughness, float n_dot_v)
{
    const float4 c0 = float4(-1.0, -0.0275, -0.572, 0.022);
    const float4 c1 = float4(1.0, 0.0425, 1.04, -0.04);
    const float4 r = roughness * c0 + c1;
    const float a004 = min(r.x * r.x, exp2(-9.28 * n_dot_v)) * r.x + r.y;
    const float2 ab = float2(-1.04, 1.04) * a004 + r.zw;
    return f0 * ab.x + ab.y;
}

float3 hemisphere(SceneFrame frame, float3 direction)
{
    return lerp(frame.ground_color.rgb, frame.sky_color.rgb, saturate(direction.y * 0.5 + 0.5));
}

float3 sky(SceneFrame frame, float3 view_direction)
{
    // A gradient toward the horizon haze, and the sun's disk with a soft glow around it.
    const float up = saturate(view_direction.y);
    const float3 horizon = lerp(frame.sky_color.rgb, frame.sun_color.rgb * 0.08, 0.35);
    const float3 gradient = lerp(horizon, frame.sky_color.rgb * 0.7, pow(up, 0.45));
    const float to_sun = saturate(dot(view_direction, frame.sun_direction.xyz));
    const float3 sun = frame.sun_color.rgb * (pow(to_sun, 4000.0) * 30.0 + pow(to_sun, 64.0) * 0.08);
    const float3 below = frame.ground_color.rgb * 0.5;
    return view_direction.y >= 0.0 ? gradient + sun : below;
}

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    const SceneFrame frame = *push.frame;
    const int2 texel = int2(input.position.xy);
    const float depth = load_texture(push.depth, texel).r;
    // The point on the far plane (or the surface) back in render-origin space.
    const float4 clip = float4(input.ndc.x, input.ndc.y, max(depth, 1e-7), 1.0);
    const float4 point = mul_rows(frame.inv_view_proj, clip);
    const float3 position = point.xyz / point.w;
    const float3 to_camera = frame.camera_position.xyz - position;
    const float3 v = normalize(to_camera);
    if (depth <= 0.0)
    {
        return float4(min(sky(frame, -v), kHalfMax), 1.0);
    }

    const GBufferSample s = unpack_gbuffer(load_texture(push.gb0, texel), load_texture(push.gb1, texel).xy,
                                           load_texture(push.gb2, texel));
    const float3 n = s.normal;
    const float3 l = frame.sun_direction.xyz;
    // Looking straight at the sun from under the surface, l + v is zero: the normal stands in.
    const float3 half_vector = l + v;
    const float3 h = dot(half_vector, half_vector) > 1e-8 ? normalize(half_vector) : n;
    const float n_dot_v = max(dot(n, v), 1e-4);
    const float n_dot_l_raw = dot(n, l);
    const float n_dot_l = saturate(n_dot_l_raw);
    const float n_dot_h = saturate(dot(n, h));
    const float v_dot_h = saturate(dot(v, h));
    const float a = max(s.roughness * s.roughness, 2e-3);
    const float3 f0 = lerp(float3(0.04), s.base_color, s.metalness);
    const float3 albedo = s.base_color * (1.0 - s.metalness);

    // Sun: specular, then diffuse (wrapped around the terminator for translucent surfaces), then
    // light seen through a translucent surface when the sun is behind it.
    const float3 fresnel = f_schlick(f0, v_dot_h);
    const float3 specular = d_ggx(n_dot_h, a) * v_smith(n_dot_v, n_dot_l, a) * fresnel * n_dot_l;
    const float wrap = s.translucency * 0.5;
    const float diffuse_term = saturate((n_dot_l_raw + wrap) / (1.0 + wrap)) / (1.0 + wrap);
    const float3 diffuse = albedo * (1.0 - fresnel) * diffuse_term / kPi;
    const float through = pow(saturate(dot(-v, l)), 12.0) * saturate(-n_dot_l_raw + 0.5);
    const float3 transmitted = albedo * s.translucency * through / kPi;
    // Shadowed sunlight keeps the shadow color (a tint, not black: the look's painterly shadows).
    const float seen = load_texture(push.shadow_mask, texel).r;
    const float3 sun_light = frame.sun_color.rgb * lerp(frame.shadow_color.rgb, float3(1.0), seen);
    const float3 sun = sun_light * (diffuse + specular + transmitted);

    // Ambient: the hemisphere for diffuse, and in the reflected direction for specular.
    const float3 r = reflect(-v, n);
    const float3 ambient = (albedo * hemisphere(frame, n) + env_brdf(f0, s.roughness, n_dot_v) * hemisphere(frame, r)) *
                           s.occlusion;
    // Mirror-smooth highlights overflow half floats (+inf, then NaN in the tonemap): kept finite.
    return float4(min(sun + ambient, kHalfMax), 1.0);
}
