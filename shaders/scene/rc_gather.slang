// The radiance cascades' light at each pixel of the GI picture (docs/design.md, Global illumination: radiance
// cascades), in compute: cascade 0, whose probes are the GI picture's pixels, marched here and never stored, merged
// with cascade 1 and gathered (scene.rc_common for the method).
//
// For each azimuth: a march over the couple of pixels round this one (the finest creases), the merge with the
// cascades above, then the light arriving through that wedge of directions. A bin of elevations holds, from its
// lowest edge up: what's behind the probe's own surface (nothing arrives), what's covered (the light of what
// covers it), and what's open (the sky's light, scene.sky's sky_light).
//
// What's kept isn't the light on the surface but the light's first two moments over directions, each colour's
// apart: how much arrives in all (the integral of the radiance over the sphere) and from where (the integral of
// the radiance times its direction), each part of a wedge in closed form. From those, the light on a surface
// facing any way n is moment 0 / 4 + (moment 1 . n) / 2 (the cosine's first two spherical harmonics: exact for a
// sky the same over a hemisphere, whichever way the surface faces). So the pass that adds the light
// (scene.rc_apply) can take it for each of the screen's pixels with that pixel's own normal, the material's normal
// map and all, from a picture smaller than the screen.
//
// The output is the GI picture's size across and three times it down: red's texels, then green's below them, then
// blue's; in a texel, x is the colour's moment 0 and yzw its moment 1.
//
// Kept beside it, for the next frame: what the surface sends on of the light gathered (its colour times the light
// on it; not of the direct light, which the lit picture has). The next frame's marches meet it with the lit
// picture (scene.rc_pyramid), so light bounces once more each frame: the sky's light off the ground reaches a
// wall's face, a lit wall's light off the floor reaches the wall across.

import common.bindless;
import scene.rc_common;
import scene.scene_common;
import scene.sky;

struct RcGatherPush
{
    SceneFrame* frame;
    uint depth;         // the pyramids (scene.rc_pyramid), sampled
    uint normal;
    uint radiance;
    uint upper;         // cascade 1, merged (sampled)
    uint output;        // the moments, as a storage texture
    uint has_upper;     // 0: there's only cascade 0
    uint2 size;         // the GI picture's
    uint2 screen;       // the screen's
    uint2 upper_probes; // cascade 1's, each way
    uint scale;         // the screen's pixels to one of the GI picture's, each way
    uint azimuths;      // cascade 0's
    uint steps;         // along its interval
    float start_px;     // the first sample's distance from the pixel
    float step_px;      // between samples
    float thickness_m;  // how far behind its surface a sample still stands
    float tolerance;    // of the merge's plane weight, in spacings of cascade 1
    float phase;        // what every azimuth is turned by
    float reserved_sky; // (the sky's share is the place's own: scene.sky's open_sky)
    float bounce;       // what the light of what's on the screen is multiplied by
    uint white_sky;     // 1: the sky is white from every way (to see what's open)
    uint upper_wide;    // 1: cascade 1 stores every azimuth (scene.radiance_cascades)
    uint base_color;    // the G-buffer's base colour and material (the screen's size), if sends_on
    uint material;
    uint sent_on;       // what each surface sends on of the light gathered, as a storage texture, if sends_on
    uint sends_on;      // bit 0: keep it; bit 1: times the material's own occlusion
};

[[vk::push_constant]]
ConstantBuffer<RcGatherPush> push;

// The integral of sqrt(1 - c^2) from 0 up to a cos: what the directions of a slice from the screen's plane up to
// that elevation add along the slice's tangent, per unit of radiance and of the wedge's chord.
float along_up_to(float c)
{
    return 0.5 * (c * sqrt(max(1.0 - c * c, 0.0)) + asin(c));
}

// along_up_to at each bin's top.
static const float4 kBinTopAlong = float4(0.785398163, 0.478305739, 0.0, -0.478305739);

// The moments of light: what arrives in all, and what arrives times the way it comes from, each colour's apart.
struct Moments
{
    float3 all;
    float3 red;
    float3 green;
    float3 blue;
};

// Adds the light of a part of a wedge: its radiance, the part's solid angle and the integral of its directions.
void add_light(inout Moments moments, float3 radiance, float solid_angle, float3 directions)
{
    moments.all += radiance * solid_angle;
    moments.red += directions * radiance.r;
    moments.green += directions * radiance.g;
    moments.blue += directions * radiance.b;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const uint2 pixel = thread.xy;
    if (any(pixel >= push.size))
    {
        return;
    }
    SceneFrame* frame = push.frame;
    RWTexture2D<float4> output = storage_2d(push.output);
    const float depth = load_texture(push.depth, int2(pixel)).r;
    if (depth <= 0.0)
    {
        // The sky: nothing to light, and no weight for the pixels round it.
        for (uint colour = 0; colour < 3; ++colour)
        {
            output[uint2(pixel.x, colour * push.size.y + pixel.y)] = float4(0.0);
        }
        if ((push.sends_on & 1u) != 0)
        {
            storage_2d(push.sent_on)[pixel] = float4(0.0);
        }
        return;
    }

    const RcRays rays = rc_rays(frame, push.screen, push.scale);
    RcProbe probe;
    probe.at = float2(pixel) + 0.5;
    probe.offset = rc_ray(rays, probe.at) / depth;
    probe.to_camera = -normalize(probe.offset);
    probe.normal = load_texture(push.normal, int2(pixel)).xyz;

    // How much of the sky's light what's open lets in: by how open to the sky this place is.
    const float sky_share =
        open_sky(frame, frame.camera_position.xyz + probe.offset + probe.normal * kOpenSkyAskedM);

    RcSources sources;
    sources.depth = push.depth;
    sources.normal = push.normal;
    sources.radiance = push.radiance;
    sources.size = push.size;
    sources.thickness_m = push.thickness_m;
    sources.solid = push.thickness_m >= kRcSolidThicknessM;

    RcUpper upper = {};
    if (push.has_upper != 0)
    {
        upper = rc_upper(rays, probe, sources, push.upper, push.upper_wide != 0, push.upper_probes, 2, push.tolerance);
    }

    const float wedge = 2.0 * kRcPi / float(push.azimuths);
    const float chord = 2.0 * sin(wedge * 0.5);
    Moments moments;
    moments.all = float3(0.0);
    moments.red = float3(0.0);
    moments.green = float3(0.0);
    moments.blue = float3(0.0);
    for (uint a = 0; a < push.azimuths; ++a)
    {
        RcMarch march;
        march.direction = rc_azimuth(a, push.azimuths, push.phase);
        march.start = push.start_px;
        march.step = push.step_px;
        march.count = push.steps;
        march.mip = 0.0;
        // Four azimuths along the picture's axes, a pixel a step: every sample is a pixel's middle.
        march.exact = push.azimuths == 4 && push.step_px == 1.0 && push.start_px == 1.0;
        const float3 tangent = rc_slice_tangent(rays, probe, march.direction);
        RcSlice slice = rc_slice_start(probe, tangent);
        const float ground = slice.horizon; // below it is behind the probe's own surface
        rc_march(slice, rays, probe, sources, march);
        if (push.has_upper != 0)
        {
            rc_merge(slice, upper, probe, tangent, a);
        }

        // A bin, from its lowest edge up: the ground (nothing arrives), what the horizon covers (its light), what's
        // open (the sky), what the floating piece covers (its light), what's open above it. The four cos between
        // them, held inside the bin, and the bin's top.
        const float horizon = max(slice.horizon, ground);
        const bool floating = rc_has_floating(slice);
        const float floating_from = floating ? max(slice.floating.x, horizon) : 2.0;
        const float floating_to = floating ? max(slice.floating.y, floating_from) : 2.0;
        // along_up_to rises with its cos, so where a cos is held inside a bin its along_up_to is the cos's own, held
        // between the bin's: four arcsines for the slice, not four a bin.
        const float ground_along = along_up_to(clamp(ground, -1.0, 1.0));
        const float horizon_along = along_up_to(clamp(horizon, -1.0, 1.0));
        const float from_along = along_up_to(clamp(floating_from, -1.0, 1.0));
        const float to_along = along_up_to(clamp(floating_to, -1.0, 1.0));
        for (uint e = 0; e < kRcElevations; ++e)
        {
            const float low = kRcBinLow[e];
            const float high = low + kRcBinWidth;
            float edges[5];
            const float low_along = e + 1 < kRcElevations ? kBinTopAlong[e + 1] : -kBinTopAlong[0];
            const float high_along = kBinTopAlong[e];
            float along[5];
            edges[0] = clamp(ground, low, high);
            along[0] = clamp(ground_along, low_along, high_along);
            edges[1] = clamp(horizon, low, high);
            along[1] = clamp(horizon_along, low_along, high_along);
            const bool lit = any(slice.light[e] > 0.0);
            if (lit)
            {
                // A bin with light holds it over at least a twentieth of itself: the cascades above laid it where
                // each of their probes saw it, and the horizon merged from them may fall a hair short of that.
                const float least = min(edges[0] + kRcBinWidth * 0.05, high);
                edges[1] = max(edges[1], least);
                along[1] = max(along[1], along_up_to(least));
            }
            edges[2] = max(clamp(floating_from, low, high), edges[1]);
            along[2] = max(clamp(from_along, low_along, high_along), along[1]);
            edges[3] = max(clamp(floating_to, low, high), edges[2]);
            along[3] = max(clamp(to_along, low_along, high_along), along[2]);
            edges[4] = high;
            along[4] = high_along;

            // What's covered: its light, spread over what of the bin is covered above the probe's surface.
            const float covered = (edges[1] - edges[0]) + (edges[3] - edges[2]);
            if (lit && covered > 0.0)
            {
                const float3 radiance = slice.light[e] * (kRcBinWidth / covered) * push.bounce;
                const float toward = (edges[1] * edges[1] - edges[0] * edges[0]) +
                                     (edges[3] * edges[3] - edges[2] * edges[2]);
                const float sideways = (along[1] - along[0]) + (along[3] - along[2]);
                add_light(moments, radiance, wedge * covered,
                          probe.to_camera * (wedge * 0.5 * toward) + tangent * (chord * sideways));
            }
            // What's open, below the floating piece and above it: the sky from the middle of each part's
            // directions (its light is linear in the direction, so that's the part's mean).
            for (uint part = 1; part < 4; part += 2)
            {
                const float open = edges[part + 1] - edges[part];
                if (open <= 1e-6)
                {
                    continue;
                }
                const float solid_angle = wedge * open;
                const float3 directions =
                    probe.to_camera * (wedge * 0.5 * (edges[part + 1] * edges[part + 1] - edges[part] * edges[part])) +
                    tangent * (chord * (along[part + 1] - along[part]));
                const float3 radiance =
                    (push.white_sky != 0 ? float3(1.0) : sky_light(frame, directions / solid_angle)) * sky_share;
                add_light(moments, radiance, solid_angle, directions);
            }
        }
    }
    output[uint2(pixel.x, pixel.y)] = float4(min(moments.all.r, kHalfMax), moments.red);
    output[uint2(pixel.x, push.size.y + pixel.y)] = float4(min(moments.all.g, kHalfMax), moments.green);
    output[uint2(pixel.x, 2 * push.size.y + pixel.y)] = float4(min(moments.all.b, kHalfMax), moments.blue);
    if ((push.sends_on & 1u) != 0)
    {
        // The light on the probe's own surface (moment 1 along its normal, over pi: what a white surface gives
        // back), times the surface's colour: its block's mean, so a texture's grain doesn't flicker in it.
        const float3 arriving =
            max(float3(dot(moments.red, probe.normal), dot(moments.green, probe.normal), dot(moments.blue, probe.normal)),
                0.0) / kRcPi;
        const float3 base = rc_block_mean(push.base_color, pixel, push.scale, push.screen).rgb;
        const float4 material = load_texture(push.material, int2(rc_screen_pixel(pixel, push.scale, push.screen)));
        const float occlusion = (push.sends_on & 2u) != 0 ? material.b : 1.0;
        storage_2d(push.sent_on)[pixel] = float4(min(base * ((1.0 - material.g) * occlusion) * arriving, kHalfMax), 1.0);
    }
}
