// Deferred lighting: one full-screen pass over the G-buffer, added onto the HDR
// target (which holds the emissive light). The sun: GGX specular (height-correlated Smith,
// Schlick Fresnel) and Lambert diffuse, wrapped and with backlight transmission for translucent
// surfaces. Ambient: light that has bounced, from the light boxes (scene.light_boxes), where one holds the
// pixel; elsewhere a sky/ground hemisphere, the placeholder for image-based lighting (neither while the radiance
// cascades light the picture: scene.rc_apply adds what they gathered, after this pass). The local
// lights of the pixel's cluster (scene.local_lights). Where nothing was drawn (depth at the far plane),
// the sky.

import common.bindless;
import scene.brdf;
import scene.light_boxes;
import scene.local_lights;
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
    uint indirect;    // 1: only the ambient that falls on each surface, whatever its colour (a debug view)
    uint ao;          // scene.gtao's occlusion, if ao_on
    uint ao_on;
    uint local_shadows; // the local shadow atlas (scene.local_lights' local_shadow); 0: none
    uint ambient_off;   // bit 0: no diffuse ambient; bit 1: no specular ambient. Both while the radiance cascades
                        // light the picture: scene.rc_apply adds the two from what they gathered. Bit 1 alone: to
                        // compare with the path tracer, which has no specular ambient yet
};

// Light bounces between the walls of a crease, so an occluded spot of a bright surface is less dark than its
// occlusion says: Jimenez et al.'s fit (the GTAO paper) from the occlusion and the surface's colour, per channel.
float3 multi_bounce(float visibility, float3 albedo)
{
    const float3 a = 2.0404 * albedo - 0.3324;
    const float3 b = -4.7951 * albedo + 0.6417;
    const float3 c = 2.7552 * albedo + 0.6903;
    return max(float3(visibility), ((visibility * a + b) * visibility + c) * visibility);
}

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
    // The surface as the depth shows it, toward the camera (taken here, before anything branches): where its sky is
    // asked about below. The material's normal turns with every bump, and a bump's normal can lean out of a hall.
    const float3 across = cross(ddy(position), ddx(position));
    const float3 face = normalize(dot(across, v) < 0.0 ? -across : across);
    if (depth <= 0.0)
    {
        if (frame.backdrop_top.w > 0.0)
        {
            // A plain backdrop: the same down the picture wherever the camera looks.
            return float4(lerp(frame.backdrop_bottom.rgb, frame.backdrop_top.rgb, input.ndc.y * 0.5 + 0.5), 1.0);
        }
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
    // Shadowed sunlight keeps the shadow color (a tint, not black: the look's painterly shadows). The tint is the
    // sky's doing, so a place closed to the sky keeps less of it (scene.sky's open_sky): under a roof it would be
    // a sun that shines through the walls, dimly, and glints off every bump seen edge on.
    const float seen = load_texture(push.shadow_mask, texel).r;
    const float3 kept = frame.shadow_color.rgb * open_sky(frame, position + face * kOpenSkyAskedM);
    const float3 sun_light = frame.sun_color.rgb * lerp(kept, float3(1.0), seen);
    // Light seen through a translucent surface isn't shadowed: the surface is its own occluder in
    // the shadow map (sun behind it), which would always put it in shadow.
    const float3 sun = sun_light * (diffuse + specular) + frame.sun_color.rgb * transmitted;

    // Ambient, for diffuse and in the reflected direction for specular: the sky/ground hemisphere, and the light
    // boxes' baked light where they hold the point. None of it while the radiance cascades add both.
    float3 ambient = float3(0.0);
    if ((push.ambient_off & 3u) != 3u)
    {
        const float3 r = reflect(-v, n);
        float3 round_normal = hemisphere(frame, n);
        float3 round_mirror = hemisphere(frame, r);
        const BoxLight boxed = light_box_light(frame, position, n, r);
        round_normal = lerp(round_normal, boxed.diffuse, boxed.weight);
        if ((push.ambient_off & 1u) != 0)
        {
            round_normal = float3(0.0);
        }
        round_mirror = lerp(round_mirror, boxed.mirror, boxed.weight);
        if ((push.ambient_off & 2u) != 0)
        {
            round_mirror = float3(0.0);
        }
        // What occludes it: its material's own (cavities in its texture) and what's near it on the screen, the
        // darker of the two (both see the same crease, at their own scales: multiplied, it would be dark twice).
        const float screen = push.ao_on != 0 ? load_texture(push.ao, texel).r : 1.0;
        const float visibility = min(s.occlusion, screen);
        const float3 diffuse_visibility = multi_bounce(visibility, albedo);
        if (push.indirect != 0)
        {
            return float4(min(round_normal * diffuse_visibility, kHalfMax), 1.0);
        }
        ambient = albedo * round_normal * diffuse_visibility +
                  env_brdf(f0, s.roughness, n_dot_v) * specular_share * round_mirror *
                      specular_occlusion(n_dot_v, visibility, s.roughness);
    }
    // The local lights that reach this pixel's cluster, each as the sun is worked out.
    LitSurface surface;
    surface.position = position;
    surface.normal = n;
    surface.to_camera = v;
    surface.albedo = albedo;
    surface.f0 = f0;
    surface.a = a;
    surface.specular_share = specular_share;
    surface.translucency = s.translucency;
    const LocalLighting local = local_lights(frame, surface, push.local_shadows, input.position.xy);
    // Mirror-smooth highlights overflow half floats (+inf, then NaN in the tonemap): kept finite.
    return float4(min(sun + ambient + local.diffuse + local.specular, kHalfMax), 1.0);
}
