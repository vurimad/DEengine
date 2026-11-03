// The sun's shadow mask (renderer step 3): one full-screen pass resolving how much of the sun each
// pixel sees, from the cascaded shadow atlas, for the lighting pass to read.
//   - The cascade by distance along the view; near a cascade's far edge, a dithered share of pixels
//     uses the next one, so the seam doesn't show (temporal filtering will smooth the dither).
//   - The receiver is pushed along its normal by about a texel (no acne on slopes) and toward the
//     sun by a fraction of one (no acne on flat ground).
//   - Contact-hardening filter: a blocker search finds how far the occluder is, which sets the
//     filter's width (sharp at the contact, softer away from it), then 16 Poisson taps rotated per
//     pixel by interleaved gradient noise.
// Output: r = sunlight seen (0..1), g = the cascade used, for the debug view.

import common.bindless;
import scene.scene_common;

struct ShadowMaskPush
{
    SceneFrame* frame;
    uint depth;
    uint gb1;
    uint atlas;
    uint sampler; // point clamp, for the raw depths of the blocker search
};

[[vk::push_constant]]
ConstantBuffer<ShadowMaskPush> push;

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

static const float2 kPoisson[16] = {
    float2(-0.94201624, -0.39906216), float2(0.94558609, -0.76890725), float2(-0.09418410, -0.92938870),
    float2(0.34495938, 0.29387760),   float2(-0.91588581, 0.45771432), float2(-0.81544232, -0.87912464),
    float2(-0.38277543, 0.27676845),  float2(0.97484398, 0.75648379),  float2(0.44323325, -0.97511554),
    float2(0.53742981, -0.47373420),  float2(-0.26496911, -0.41893023), float2(0.79197514, 0.19090188),
    float2(-0.24188840, 0.99706507),  float2(-0.81409955, 0.91437590), float2(0.19984126, 0.78641367),
    float2(0.14383161, -0.14100790),
};

// Jimenez's interleaved gradient noise: a well-spread value per pixel, in [0, 1).
float gradient_noise(float2 pixel)
{
    return frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

// A cascade's uv in the atlas, kept inside its tile (a texel of border) so filters don't bleed.
float2 atlas_uv(SceneFrame frame, uint cascade, float2 tile_uv)
{
    const float border = frame.shadow_params.y * 2.0;
    return cascade_tile(cascade) + clamp(tile_uv, border, 1.0 - border) * 0.5;
}

[shader("fragment")]
float2 fragment_main(VertexOutput input) : SV_Target
{
    const SceneFrame frame = *push.frame;
    const int2 texel = int2(input.position.xy);
    const float depth = load_texture(push.depth, texel).r;
    if (depth <= 0.0)
    {
        return float2(1.0, 0.0); // the sky
    }
    const float4 point = mul_rows(frame.inv_view_proj, float4(input.ndc, depth, 1.0));
    float3 position = point.xyz / point.w;
    const float distance = dot(position - frame.camera_position.xyz, frame.camera_forward.xyz);

    // The cascade: the first that reaches this far; the dithered band hands some pixels on.
    uint cascade = kCascadeCount;
    for (uint c = 0; c < kCascadeCount; ++c)
    {
        if (distance < frame.cascade_far[c])
        {
            cascade = c;
            break;
        }
    }
    if (cascade == kCascadeCount)
    {
        return float2(1.0, 1.0); // beyond the shadow distance: lit
    }
    const float noise = gradient_noise(input.position.xy);
    const float start = cascade == 0 ? 0.0 : frame.cascade_far[cascade - 1];
    const float band_start = lerp(start, frame.cascade_far[cascade], 1.0 - frame.shadow_params.x);
    const float into_band = saturate((distance - band_start) / max(frame.cascade_far[cascade] - band_start, 1e-4));
    // The last cascade fades to lit over its band, so the shadow distance has no hard line (only
    // pixels in it to begin with: one handed on from the cascade before isn't in its band).
    const float fade_out = cascade == kCascadeCount - 1 ? into_band : 0.0;
    if (cascade + 1 < kCascadeCount && noise < into_band)
    {
        ++cascade;
    }

    const float3 normal = octahedral_decode(load_texture(push.gb1, texel).xy * 2.0 - 1.0);
    const float texel_m = frame.cascade_texel_m[cascade];
    position += normal * texel_m * 1.5 + frame.sun_direction.xyz * texel_m * 0.5;
    const float4 clip = cascade_clip(frame, cascade, position);
    const float2 tile_uv = float2(clip.x * 0.5 + 0.5, 0.5 - clip.y * 0.5);
    const float receiver = clip.z;
    const float atlas_texel = frame.shadow_params.y;

    // Blocker search: the mean depth of what's nearer the sun, around the receiver.
    const float angle = noise * 6.2831853;
    const float2x2 rotation = float2x2(cos(angle), -sin(angle), sin(angle), cos(angle));
    const float search = frame.shadow_params.w * atlas_texel;
    float blockers = 0.0;
    float blocker_depth = 0.0;
    for (uint i = 0; i < 8; ++i)
    {
        const float2 uv = atlas_uv(frame, cascade, tile_uv + mul(rotation, kPoisson[i * 2]) * search * 2.0);
        const float stored = sample_texture_level(push.atlas, push.sampler, uv, 0.0).r;
        if (stored > receiver)
        {
            blocker_depth += stored;
            blockers += 1.0;
        }
    }
    if (blockers == 0.0)
    {
        return float2(1.0, (float(cascade) + 0.5) / 4.0);
    }
    blocker_depth /= blockers;
    // How far the occluder is above the receiver, in meters, sets the penumbra (in atlas texels).
    const float gap_m = (blocker_depth - receiver) / frame.cascade_depth_per_m[cascade];
    const float penumbra = clamp(gap_m * frame.shadow_params.z / texel_m, 1.0, frame.shadow_params.w);

    float lit = 0.0;
    for (uint i = 0; i < 16; ++i)
    {
        const float2 uv = atlas_uv(frame, cascade, tile_uv + mul(rotation, kPoisson[i]) * penumbra * atlas_texel * 2.0);
        lit += sample_shadow(push.atlas, uv, receiver);
    }
    lit /= 16.0;
    return float2(lerp(lit, 1.0, fade_out), (float(cascade) + 0.5) / 4.0);
}
