// Deferred lighting: one full-screen pass over the G-buffer, added onto the HDR
// target (which holds the emissive light). The sun: GGX specular (height-correlated Smith,
// Schlick Fresnel) and Lambert diffuse, wrapped and with backlight transmission for translucent
// surfaces. Ambient: a sky/ground hemisphere, the placeholder for image-based lighting. Where
// nothing was drawn (depth at the far plane), the sky.

import common.bindless;
import scene.brdf;
import scene.scene_common;
import scene.sky;

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

[shader("fragment")]
float4 fragment_main(VertexOutput input) : SV_Target
{
    SceneFrame* frame = push.frame;
    const int2 texel = int2(input.position.xy);
    const float depth = load_texture(push.depth, texel).r;
    // The point on the far plane (or the surface) back in render-origin space.
    const float3 position = position_from_depth(frame, input.ndc, depth);
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
    const float specular_share = s.lighting_group == kLightingGroupGround ? kGroundSpecularShare : 1.0;
    const float3 specular = d_ggx(n_dot_h, a) * v_smith(n_dot_v, n_dot_l, a) * fresnel * n_dot_l * specular_share;
    const float wrap = s.translucency * 0.5;
    const float diffuse_term = saturate((n_dot_l_raw + wrap) / (1.0 + wrap)) / (1.0 + wrap);
    const float3 diffuse = albedo * (1.0 - fresnel) * diffuse_term / kPi;
    const float through = pow(saturate(dot(-v, l)), 12.0) * saturate(-n_dot_l_raw + 0.5);
    const float3 transmitted = albedo * s.translucency * through / kPi;
    // Shadowed sunlight keeps the shadow color (a tint, not black: the look's painterly shadows).
    const float seen = load_texture(push.shadow_mask, texel).r;
    const float3 sun_light = frame.sun_color.rgb * lerp(frame.shadow_color.rgb, float3(1.0), seen);
    // Light seen through a translucent surface isn't shadowed: the surface is its own occluder in
    // the shadow map (sun behind it), which would always put it in shadow.
    const float3 sun = sun_light * (diffuse + specular) + frame.sun_color.rgb * transmitted;

    // Ambient: the hemisphere for diffuse, and in the reflected direction for specular.
    const float3 r = reflect(-v, n);
    const float3 ambient = (albedo * hemisphere(frame, n) + env_brdf(f0, s.roughness, n_dot_v) * specular_share * hemisphere(frame, r)) *
                           s.occlusion;
    // Mirror-smooth highlights overflow half floats (+inf, then NaN in the tonemap): kept finite.
    return float4(min(sun + ambient, kHalfMax), 1.0);
}
