// What the radiance cascades' marches read (docs/design.md, Global illumination: radiance cascades), in compute:
// three pyramids of the GI picture (scene.rc_common: the screen at 1 / scale of its size each way), a mip a
// dispatch.
//   - depth: the scene's reverse-Z depth (the sky is 0). Reverse-Z depth is linear across the screen on a flat
//     surface, so a mip's mean, and a sample between texels, is still on the surface.
//   - normal: of the surface as the depth shows it (not the material's normal map: the marches are about what
//     stands where), toward the camera. A mip's mean is shorter where the surfaces under it disagree. In its alpha,
//     the share of the surface's light that it emits itself (scene.rc_glow against the lit picture): its unseen
//     sides are taken to glow alike.
//   - radiance: the lit picture (the deferred light's: direct light and glow), with how much of the texel is
//     surface in alpha; the sky is 0 there and in rgb. So rgb over alpha is the light of the surfaces alone.
//     With it, light that has bounced before: what each surface sent on last frame of the light the cascades
//     gathered there (scene.rc_gather keeps it), read where the surface was on the screen then. A frame adds a
//     bounce, so a still picture holds them all after a few; what moves is lit a frame late by what has bounced.
// A far sample reads a coarse mip: the light of everything under it, so a small bright thing is never stepped over.

import common.bindless;
import scene.rc_common;
import scene.scene_common;

struct RcPyramidPush
{
    SceneFrame* frame;
    uint depth_in;     // mip 0: the scene's depth (sampled); after it, the mip before (a storage texture)
    uint radiance_in;  // mip 0: the lit picture (sampled); after it, the mip before (a storage texture)
    uint normal_in;    // the mip before (a storage texture); unused at mip 0
    uint depth_out;    // this mip of each, as storage textures
    uint normal_out;
    uint radiance_out;
    uint2 size;        // this mip's
    uint2 source_size; // the mip before's (mip 0: the scene's)
    uint mip;
    uint glow;         // mip 0: what's emitted, at the GI picture's size (scene.rc_glow, sampled)
    uint scale;        // mip 0: the screen's pixels to one of the GI picture's, each way
    uint bounce;       // mip 0: what surfaces sent on last frame, at the GI picture's size (sampled), if has_bounce
    uint has_bounce;
    uint reserved;
};

[[vk::push_constant]]
ConstantBuffer<RcPyramidPush> push;

float depth_at(int2 pixel)
{
    return load_texture(push.depth_in, clamp(pixel, int2(0), int2(push.source_size) - 1)).r;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const uint2 texel = thread.xy;
    if (any(texel >= push.size))
    {
        return;
    }
    RWTexture2D<float4> depth_out = storage_2d(push.depth_out);
    RWTexture2D<float4> normal_out = storage_2d(push.normal_out);
    RWTexture2D<float4> radiance_out = storage_2d(push.radiance_out);
    if (push.mip == 0)
    {
        // The GI picture (scene.rc_common): a pixel is one of the screen's for its depth and its surface (a mean
        // of depths is a place on no surface, and a probe stands on a pixel), and the mean of its block of the
        // screen for its light, so a spark a pixel wide is never stepped over and doesn't flicker as it moves.
        const uint scale = push.scale;
        const int2 pixel = int2(rc_screen_pixel(texel, scale, push.source_size));
        const float depth = depth_at(pixel);
        depth_out[texel] = float4(depth, 0.0, 0.0, 0.0);
        if (depth <= 0.0)
        {
            normal_out[texel] = float4(0.0);
            radiance_out[texel] = float4(0.0);
            return;
        }
        // How much of its block is surface. All of it, nearly always: then the block's light is its plain mean.
        // Else (the sky in a part of it, whose colour the lit picture holds) only the surfaces' light is summed.
        float surfaces = 0.0;
        for (uint y = 0; y < scale; ++y)
        {
            for (uint x = 0; x < scale; ++x)
            {
                surfaces += depth_at(int2(min(texel * scale + uint2(x, y), push.source_size - 1))) > 0.0 ? 1.0 : 0.0;
            }
        }
        const float share = 1.0 / float(scale * scale);
        float3 light; // the mean over the block, the sky's part counting as none
        if (surfaces * share > 0.999)
        {
            // Kept finite and positive: one bad pixel would spread through every mip.
            light = clamp(rc_block_mean(push.radiance_in, texel, scale, push.source_size).rgb, 0.0, kHalfMax);
        }
        else
        {
            light = float3(0.0);
            for (uint y = 0; y < scale; ++y)
            {
                for (uint x = 0; x < scale; ++x)
                {
                    const int2 at = int2(min(texel * scale + uint2(x, y), push.source_size - 1));
                    if (depth_at(at) > 0.0)
                    {
                        light += clamp(load_texture(push.radiance_in, at).rgb, 0.0, kHalfMax);
                    }
                }
            }
            light *= share;
        }
        // The screen's own rays: where this pixel's surface is.
        const RcRays rays = rc_rays(push.frame, push.source_size, 1);
        const float2 at = float2(pixel) + 0.5;
        const float3 here = rc_ray(rays, at) / depth;
        if (push.has_bounce != 0)
        {
            // What this surface sent on last frame, where it was on the screen then (a thing that moved, or one
            // that wasn't on the screen, is lit a frame late or not at all by it: it's the broadest of the light).
            const float4 clip = mul_rows(push.frame.previous_view_proj, float4(push.frame.camera_position.xyz + here, 1.0));
            const float2 uv = float2(clip.x * 0.5, clip.y * -0.5) / max(clip.w, 1e-4) + 0.5;
            if (clip.w > 1e-4 && all(uv >= 0.0) && all(uv <= 1.0))
            {
                // The GI picture is the screen's size over the scale, rounded up: its last pixels may hang over.
                const float2 there = uv * float2(push.source_size) / (float2(push.size) * float(scale));
                light += clamp(sample_texture_level(push.bounce, kSamplerLinearClamp, there, 0.0).rgb, 0.0, kHalfMax) *
                         (surfaces * share);
            }
        }
        // As the mips are: the light times the share of the block that's a surface, and that share.
        radiance_out[texel] = float4(light, surfaces * share);
        // What it emits (the mean over the block too: the sky emits nothing) against its light.
        const float glow = load_texture(push.glow, int2(texel)).r;
        const float glows = saturate(glow / max(dot(light, float3(0.2126, 0.7152, 0.0722)), 1e-6)) * surfaces * share;

        // The surface's normal from the screen's depth: across each axis, the neighbour whose depth is nearer this
        // one's is on the same surface if either is.
        const float left = depth_at(pixel + int2(-1, 0));
        const float right = depth_at(pixel + int2(1, 0));
        const float up = depth_at(pixel + int2(0, -1));
        const float down = depth_at(pixel + int2(0, 1));
        float3 across;
        if (abs(right - depth) <= abs(left - depth))
        {
            across = rc_ray(rays, at + float2(1.0, 0.0)) / max(right, 1e-7) - here;
        }
        else
        {
            across = here - rc_ray(rays, at + float2(-1.0, 0.0)) / max(left, 1e-7);
        }
        float3 along;
        if (abs(down - depth) <= abs(up - depth))
        {
            along = rc_ray(rays, at + float2(0.0, 1.0)) / max(down, 1e-7) - here;
        }
        else
        {
            along = here - rc_ray(rays, at + float2(0.0, -1.0)) / max(up, 1e-7);
        }
        float3 normal = cross(across, along);
        const float length2 = dot(normal, normal);
        normal = length2 > 0.0 ? normal * rsqrt(length2) : -normalize(here);
        if (dot(normal, here) > 0.0)
        {
            normal = -normal;
        }
        normal_out[texel] = float4(normal, glows);
        return;
    }
    RWTexture2D<float4> depth_in = storage_2d(push.depth_in);
    RWTexture2D<float4> normal_in = storage_2d(push.normal_in);
    RWTexture2D<float4> radiance_in = storage_2d(push.radiance_in);
    const uint2 last = push.source_size - 1u;
    float depth = 0.0;
    float4 normal = float4(0.0);
    float4 radiance = float4(0.0);
    for (uint i = 0; i < 4; ++i)
    {
        const uint2 source = min(texel * 2u + uint2(i & 1u, i >> 1u), last);
        depth += depth_in[source].r;
        normal += normal_in[source];
        radiance += radiance_in[source];
    }
    depth_out[texel] = float4(depth * 0.25, 0.0, 0.0, 0.0);
    normal_out[texel] = normal * 0.25;
    radiance_out[texel] = radiance * 0.25;
}
