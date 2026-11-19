// One radiance cascade, marched and merged (docs/design.md, Global illumination: radiance cascades), in compute: a
// thread a probe and a stored azimuth (scene.rc_common for the layout and the method).
//
// A stored azimuth is the mean of two of the cascade's own: for each, a march along the screen over the cascade's
// interval, then the merge with the cascade above (already merged with every one above it), so what's stored is
// everything from the interval's start outward. The topmost merges with nothing: what's past it is the sky, which
// the gather adds (scene.rc_gather), through what no cascade covered.
//
// A cascade of few probes is "wide": a thread is a probe and one of the cascade's own azimuths, and every azimuth
// is stored. Such a pass has too few threads to fill the GPU, so it takes as long as one thread does, and a thread
// that marches and merges one azimuth takes half as long as one that does two. The cascade below takes the mean of
// the two as it reads them (scene.rc_common's rc_merge).
//
// Everything is marched again every frame; nothing is kept from the frame before.

import common.bindless;
import scene.rc_common;
import scene.scene_common;

struct RadianceCascadePush
{
    SceneFrame* frame;
    uint depth;         // the pyramids (scene.rc_pyramid), sampled
    uint normal;
    uint radiance;
    uint upper;         // the cascade above, merged (sampled); unused for the topmost
    uint output;        // this cascade's, as a storage texture
    uint top;           // bit 0: the topmost cascade; bit 1: this one is wide; bit 2: the one above is
    uint2 size;         // the GI picture's (scene.rc_common)
    uint2 probes;       // this cascade's, each way
    uint2 upper_probes; // the one above's
    uint spacing;       // between this cascade's probes, in pixels
    uint azimuths;      // this cascade's: twice what's stored
    float start_px;     // the first sample's distance from the probe
    float step_px;      // between samples
    uint steps;
    float mip;          // of the pyramids, to read
    float thickness_m;  // how far behind its surface a sample still stands
    float tolerance;    // of the merge's plane weight, in spacings of the cascade above
    float phase;        // what every azimuth is turned by
    uint scale;         // the screen's pixels to one of the GI picture's, each way
    uint2 screen;       // the screen's size
};

[[vk::push_constant]]
ConstantBuffer<RadianceCascadePush> push;

[shader("compute")]
[numthreads(8, 8, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const bool top = (push.top & 1u) != 0;
    const bool wide = (push.top & 2u) != 0;
    const uint stored = wide ? push.azimuths : push.azimuths / 2;
    const uint azimuth = thread.x / push.probes.x;
    const uint2 probe_index = uint2(thread.x % push.probes.x, thread.y);
    if (azimuth >= stored || probe_index.y >= push.probes.y)
    {
        return;
    }
    RWTexture2D<float4> output = storage_2d(push.output);
    const uint2 pixel = rc_probe_pixel(probe_index, push.spacing, push.size);
    const float depth = load_texture(push.depth, int2(pixel)).r;
    if (depth <= 0.0)
    {
        // The sky: no probe here, and no weight in the merge below.
        for (uint e = 0; e < kRcElevations; ++e)
        {
            output[uint2(thread.x, e * push.probes.y + probe_index.y)] = float4(0.0);
        }
        return;
    }
    const RcRays rays = rc_rays(push.frame, push.screen, push.scale);
    RcProbe probe;
    probe.at = float2(pixel) + 0.5;
    probe.offset = rc_ray(rays, probe.at) / depth;
    probe.to_camera = -normalize(probe.offset);
    probe.normal = load_texture(push.normal, int2(pixel)).xyz;

    RcSources sources;
    sources.depth = push.depth;
    sources.normal = push.normal;
    sources.radiance = push.radiance;
    sources.size = push.size;
    sources.thickness_m = push.thickness_m;
    sources.solid = push.thickness_m >= kRcSolidThicknessM;

    RcUpper upper = {};
    if (!top)
    {
        upper = rc_upper(rays, probe, sources, push.upper, (push.top & 4u) != 0, push.upper_probes, push.spacing * 2,
                         push.tolerance);
    }

    float4 kept[kRcElevations];
    for (uint e = 0; e < kRcElevations; ++e)
    {
        kept[e] = float4(0.0);
    }
    const uint children = wide ? 1 : 2;
    for (uint child = 0; child < children; ++child)
    {
        const uint own = wide ? azimuth : azimuth * 2 + child;
        RcMarch march;
        march.direction = rc_azimuth(own, push.azimuths, push.phase);
        march.start = push.start_px;
        march.step = push.step_px;
        march.count = push.steps;
        march.mip = push.mip;
        march.exact = false;
        const float3 tangent = rc_slice_tangent(rays, probe, march.direction);
        RcSlice slice = rc_slice_start(probe, tangent);
        rc_march(slice, rays, probe, sources, march);
        if (!top)
        {
            rc_merge(slice, upper, probe, tangent, own);
        }
        for (uint e = 0; e < kRcElevations; ++e)
        {
            kept[e] += rc_stored(slice, e) / float(children);
        }
    }
    for (uint e = 0; e < kRcElevations; ++e)
    {
        output[uint2(thread.x, e * push.probes.y + probe_index.y)] =
            float4(min(kept[e].rgb, kHalfMax), kept[e].a);
    }
}
