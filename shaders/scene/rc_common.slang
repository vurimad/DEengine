// What the radiance cascades' passes share (docs/design.md, Global illumination: radiance cascades): the layout of a
// cascade, a march along the screen, and the merge of a cascade with the one above it. After Sannikov's radiance
// cascades as Path of Exile 2 uses them on the screen (his paper and his ExileCon 2023 and 2026 talks), in our own
// words and code.
//
// The shape of it. Light arriving at a point is kept by direction, and a direction is two numbers here:
//   - an azimuth: a direction on the screen, round the pixel. Every ray that leaves a point along one azimuth
//     crosses the same pixels, so one march along those pixels serves them all;
//   - an elevation: the angle in that slice from the way to the camera (cos 1) down through the screen's own plane
//     (cos 0) to straight away from the camera (cos -1), kept as kRcElevations bins of equal solid angle.
// A cascade is a grid of probes over the GI picture (the screen, smaller: RcRays below). Cascade 0 has a probe a
// pixel of it, few azimuths and a reach of a couple of pixels; each next has probes twice as far apart each way, twice the azimuths, and an interval twice as
// long and as far out (the paper's penumbra condition: what's far needs fine angles and coarse positions, what's
// near the reverse). So each cascade holds half the texels of the one before, and all of them together under twice
// the first.
//
// What a slice holds isn't a hit or a miss per ray but what's covered, as pieces of its elevations. Along an azimuth
// each pixel is a column of the scene: it stands from its surface back along its own ray for a thickness, and hides
// the elevations between its front and its back. A slice keeps two pieces of what's hidden:
//   - the horizon: everything from straight away from the camera up to a cos. It starts at the probe's own tangent
//     plane (what's behind the surface it stands on) and rises as the columns that stand on it do: a wall ahead.
//     With it, how near what it rose to is (one over its distance, weighed by how far each thing raised it), so a
//     probe elsewhere can tell where it would see that;
//   - one floating piece above that: what hides elevations without reaching down to the horizon yet. The back of a
//     pillar between the probe and the camera, met from its top down, until its foot lands on the horizon.
// And per bin of elevations, the light of what newly covered it, times the share of the bin each piece covered.
// The light of a bin lies in what's covered above the probe's own tangent plane. Pieces, not hits: nothing
// flickers as a thing crosses a bin's edge, and a flat floor never shadows itself.
//
// A cascade's texture is laid out by direction first: the texel of probe (x, y), stored azimuth a and elevation e
// is (a * probes.x + x, e * probes.y + y). What's stored is the mean of a cascade's two azimuths for each azimuth
// of the cascade below (the paper's pre-averaging): half the texels, and the merge below reads one, not two. (A
// cascade of few probes stores every azimuth instead, and the merge below takes the mean of the two as it reads
// them: scene.radiance_cascades has why.) A
// texel's rgb is its bin's light; the alphas of a slice's four texels are its horizon, its floating piece's length,
// that length times the piece's middle (so a mean of slices is a mean of pieces), and the horizon's rise times its
// nearness (so a slice whose horizon didn't rise doesn't count in the mean of how near things are).

import common.bindless;
import scene.scene_common;

public static const uint kRcElevations = 4;
// The bins' lowest edges, as cos of the angle from the way to the camera; each is 2 / kRcElevations wide.
public static const float4 kRcBinLow = float4(0.5, 0.0, -0.5, -1.0);
public static const float kRcBinWidth = 0.5;
public static const float kRcPi = 3.14159265358979;
// A piece whose foot is within this cos of the horizon stands on it: surfaces and their normals aren't exact, and a
// piece kept apart from the horizon by a hair's gap doesn't average with one that isn't.
public static const float kRcGap = 0.03;

// The camera's rays over a picture: the ray through a place in pixels (a pixel's middle is at +0.5), as long as
// reaches the near plane, so a point at reverse-Z depth d is camera + ray / d. Exactly affine in the place.
//
// The cascades work on a picture `scale` times smaller each way than the screen (scene.rc_pyramid: the GI
// picture): its pixel (x, y) is the screen's pixel (x, y) * scale + scale / 2, so the middle of the one is the
// middle of the other, and every place below is in the GI picture's pixels.
public struct RcRays
{
    public float3 camera;
    public float3 origin; // the ray through the picture's top-left corner
    public float3 dx;     // what a pixel to the right adds
    public float3 dy;     // what a pixel down adds
};

float3 rc_near_point(SceneFrame* frame, float2 ndc)
{
    const float4 point = mul_rows(frame.inv_view_proj, float4(ndc, 1.0, 1.0));
    return point.xyz / point.w;
}

// `screen` is the screen's size in its own pixels; with a scale of 1 the picture is the screen.
public RcRays rc_rays(SceneFrame* frame, uint2 screen, uint scale)
{
    RcRays rays;
    rays.camera = frame.camera_position.xyz;
    const float3 corner = rc_near_point(frame, float2(-1.0, 1.0));
    const float3 dx = (rc_near_point(frame, float2(1.0, 1.0)) - corner) / float(screen.x);
    const float3 dy = (rc_near_point(frame, float2(-1.0, -1.0)) - corner) / float(screen.y);
    // The picture's pixel 0 has its middle where the screen's pixel scale / 2 has its.
    const float shift = float(scale / 2) + 0.5 - 0.5 * float(scale);
    rays.origin = corner - rays.camera + (dx + dy) * shift;
    rays.dx = dx * float(scale);
    rays.dy = dy * float(scale);
    return rays;
}

// The screen's pixel a pixel of the GI picture stands for.
public uint2 rc_screen_pixel(uint2 pixel, uint scale, uint2 screen)
{
    return min(pixel * scale + scale / 2, screen - 1);
}

public float3 rc_ray(RcRays rays, float2 at)
{
    return rays.origin + rays.dx * at.x + rays.dy * at.y;
}

// The mean of a texture of the screen's size over the block of the screen a pixel of the GI picture covers, by as
// few filtered samples as give exactly that: a sample between pixels weighs them by how near each is, so one in the
// middle of four is their mean (a scale of 2: one sample; of 4: four), and two a third of the way in from each end
// of three weigh the three alike (a scale of 3: four samples). At the screen's far edges a block that hangs over
// them counts the edge's pixels again.
public float4 rc_block_mean(uint texture, uint2 pixel, uint scale, uint2 screen)
{
    if (scale == 1)
    {
        return load_texture(texture, int2(pixel));
    }
    const uint taps = (scale + 1) / 2;
    const float first = scale == 3 ? 5.0 / 6.0 : 1.0;
    const float apart = scale == 3 ? 4.0 / 3.0 : 2.0;
    const float2 corner = float2(pixel * scale);
    float4 sum = float4(0.0);
    for (uint y = 0; y < taps; ++y)
    {
        for (uint x = 0; x < taps; ++x)
        {
            const float2 at = corner + first + apart * float2(float(x), float(y));
            sum += sample_texture_level(texture, kSamplerLinearClamp, at / float2(screen), 0.0);
        }
    }
    return sum / float(taps * taps);
}

// The pixel a probe of a cascade stands on: the middle of its spacing x spacing block.
public uint2 rc_probe_pixel(uint2 probe, uint spacing, uint2 size)
{
    return min(probe * spacing + spacing / 2, size - 1);
}

// An azimuth's direction on the screen, in pixels (y down).
public float2 rc_azimuth(uint index, uint count, float phase)
{
    const float angle = phase + 2.0 * kRcPi * (float(index) + 0.5) / float(count);
    return float2(cos(angle), sin(angle));
}

// A probe: where it stands and how its slices are measured.
public struct RcProbe
{
    public float2 at;        // its place in pixels
    public float3 offset;    // from the camera to it
    public float3 to_camera; // unit
    public float3 normal;    // of the surface it stands on, as the depth shows it
};

// The slice of an azimuth: the unit vector that, with the way to the camera, spans the plane the azimuth's rays lie
// in, pointing the azimuth's way.
public float3 rc_slice_tangent(RcRays rays, RcProbe probe, float2 direction)
{
    const float3 along = rays.dx * direction.x + rays.dy * direction.y;
    return normalize(along - probe.to_camera * dot(along, probe.to_camera));
}

// The cos of the elevation where a surface's own tangent plane cuts a slice: below it is behind the surface.
public float rc_tangent_cos(float3 normal, float3 to_camera, float3 tangent)
{
    const float along = dot(normal, tangent);
    const float toward = dot(normal, to_camera);
    return -along * rsqrt(max(along * along + toward * toward, 1e-12));
}

// The share of each bin that lies between two cos; none where the second isn't above the first.
public float4 rc_share(float from, float to)
{
    return max(min(float4(to), kRcBinLow + kRcBinWidth) - max(float4(from), kRcBinLow), 0.0) / kRcBinWidth;
}

// What a march has found along one azimuth (the top of this file).
public struct RcSlice
{
    public float horizon;               // what's covered from below reaches up to this cos
    public float risen;                 // each rise of the horizon, in cos, times one over how far what raised it
                                        // is, in meters: over the whole rise, how near what it rose to is
    public float2 floating;             // what's covered apart from that: from x up to y; nothing if y isn't above x
    public float3 light[kRcElevations]; // the radiance of what covers a bin, times the share each piece covered
};

public bool rc_has_floating(RcSlice slice)
{
    return slice.floating.y > slice.floating.x;
}

// A floating piece whose foot has reached the horizon is part of it; `nearness` is how near it's taken to be.
void rc_land(inout RcSlice slice, float nearness)
{
    if (rc_has_floating(slice) && slice.floating.x <= slice.horizon + kRcGap)
    {
        if (slice.floating.y > slice.horizon)
        {
            slice.risen += (slice.floating.y - slice.horizon) * nearness;
            slice.horizon = slice.floating.y;
        }
        slice.floating = float2(0.0);
    }
}

// A slice before its march: what's below the probe's own surface is covered, and holds no light.
public RcSlice rc_slice_start(RcProbe probe, float3 tangent)
{
    RcSlice slice;
    slice.horizon = rc_tangent_cos(probe.normal, probe.to_camera, tangent);
    slice.risen = 0.0;
    slice.floating = float2(0.0);
    for (uint e = 0; e < kRcElevations; ++e)
    {
        slice.light[e] = float3(0.0);
    }
    return slice;
}

public struct RcMarch
{
    public float2 direction; // on the screen, unit
    public float start;      // the first sample's distance from the probe, in pixels
    public float step;       // between samples
    public uint count;
    public float mip;        // of the pyramids, to read
    public bool exact;       // its samples fall on the middles of the first mip's texels: they're read as they are,
                             // not filtered from four (cascade 0's, along the picture's axes a pixel apart)
};

public struct RcSources
{
    public uint depth;    // the pyramids (scene.rc_pyramid), sampled
    public uint normal;
    public uint radiance;
    public uint2 size;    // the GI picture's
    public float thickness_m;
    public bool solid;    // the thickness is a height field's: a column stands back from its surface without end
};

// Past this a thickness is a height field's (RcSources.solid): the default is.
public static const float kRcSolidThicknessM = 50.0;

// What a march makes of leaving the picture. Nothing is known past the edge, and taking that for open lets the sky
// in from under the screen: a wall's face at the picture's foot would be lit from below, through where the ground
// goes on unseen, and lit otherwise as soon as it borrowed the light of a probe on that ground (rc_upper), which
// does take the ground to go on. So the surface the march's line crosses the edge on is taken to go on as its
// own plane: a probe standing in front of that plane has it hiding everything under the plane's way through the
// slice, with the surface's light. A probe on that plane, or behind it, is hidden nothing, and the sky at the
// edge goes on as sky.
void rc_past_edge(inout RcSlice slice, RcRays rays, RcProbe probe, RcSources sources, RcMarch march, float2 covered)
{
    const float2 size = float2(sources.size);
    const float2 room = lerp(probe.at, size - probe.at, step(float2(0.0), march.direction)) /
                        max(abs(march.direction), float2(1e-6));
    const float2 edge = clamp(probe.at + march.direction * min(room.x, room.y), float2(0.5), size - 0.5);
    const float2 uv = edge / covered;
    const int2 texel = int2(edge);
    const float depth = march.exact ? load_texture(sources.depth, texel).r
                                    : sample_texture_level(sources.depth, kSamplerLinearClamp, uv, march.mip).r;
    if (depth <= 0.0)
    {
        return; // the sky
    }
    const float4 surface = march.exact ? load_texture(sources.normal, texel)
                                       : sample_texture_level(sources.normal, kSamplerLinearClamp, uv, march.mip);
    const float3 normal = surface.xyz * rsqrt(max(dot(surface.xyz, surface.xyz), 1e-12));
    const float3 to_it = rc_ray(rays, edge) / depth - probe.offset;
    const float nearness = rsqrt(max(dot(to_it, to_it), 1e-12));
    // How far in front of that surface the probe stands: fully, by a seventh of the way to it (as rc_upper).
    const float in_front = saturate((-dot(normal, to_it) * nearness - 0.05) * 10.0);
    const float3 tangent = rc_slice_tangent(rays, probe, march.direction);
    const float to = lerp(slice.horizon, rc_tangent_cos(normal, probe.to_camera, tangent), in_front);
    if (to <= slice.horizon)
    {
        return;
    }
    float4 hidden = rc_share(slice.horizon, to);
    if (rc_has_floating(slice))
    {
        hidden = max(hidden - rc_share(max(slice.horizon, slice.floating.x), min(to, slice.floating.y)), 0.0);
    }
    slice.risen += (to - slice.horizon) * nearness;
    slice.horizon = to;
    rc_land(slice, nearness);
    const float4 radiance = march.exact ? load_texture(sources.radiance, texel)
                                        : sample_texture_level(sources.radiance, kSamplerLinearClamp, uv, march.mip);
    if (radiance.a <= 1e-3)
    {
        return;
    }
    const float3 light = radiance.rgb / radiance.a;
    for (uint e = 0; e < kRcElevations; ++e)
    {
        slice.light[e] += light * hidden[e];
    }
}

// Marches a slice outward over its interval. Each sample is a column of the picture, hiding the elevations between
// its front and its back. What of that wasn't hidden already takes the column's light, if its surface faces the
// probe: the back of a thing, or its unseen side, is dark, but for what it emits itself, which it's taken to emit
// from every side.
public void rc_march(inout RcSlice slice, RcRays rays, RcProbe probe, RcSources sources, RcMarch march)
{
    const float2 size = float2(sources.size);
    // What the mip read covers of the picture: its own size times its texels' width. A mip of a size that
    // isn't even is half of it rounded down, and still starts at the corner: read by the picture's own size,
    // its texels would be taken for wider than they are, and a far sample land beside where it's meant.
    const float2 covered = float2(max(sources.size >> uint(march.mip), uint2(1))) * exp2(march.mip);
    for (uint k = 0; k < march.count; ++k)
    {
        if (slice.horizon >= 0.9999)
        {
            break; // everything is covered
        }
        const float2 at = probe.at + march.direction * (march.start + march.step * float(k));
        if (any(at < 0.0) || any(at >= size))
        {
            rc_past_edge(slice, rays, probe, sources, march, covered);
            break; // off the picture: what stood at its edge is taken to go on
        }
        const float2 uv = at / covered;
        const int2 texel = int2(at);
        const float depth = march.exact ? load_texture(sources.depth, texel).r
                                        : sample_texture_level(sources.depth, kSamplerLinearClamp, uv, march.mip).r;
        const float3 ray = rc_ray(rays, at);
        const float3 to_front = ray / max(depth, 1e-7) - probe.offset;
        const float front_inv = rsqrt(max(dot(to_front, to_front), 1e-12));
        const float front = dot(to_front, probe.to_camera) * front_inv;
        if (front <= slice.horizon)
        {
            continue; // below what's already covered
        }
        // Its back: straight away from the camera for a height field's column, which reaches down to any horizon.
        float back = -1.0;
        if (!sources.solid)
        {
            const float3 to_back = to_front + normalize(ray) * sources.thickness_m;
            back = min(dot(to_back, probe.to_camera) * rsqrt(max(dot(to_back, to_back), 1e-12)), front);
        }
        const float low = max(back, slice.horizon);
        // What of the column's elevations is newly hidden: not inside the floating piece already.
        float4 covered = rc_share(low, front);
        const bool floating = rc_has_floating(slice);
        if (floating)
        {
            covered = max(covered - rc_share(max(low, slice.floating.x), min(front, slice.floating.y)), 0.0);
        }
        if (back <= slice.horizon + kRcGap)
        {
            slice.risen += (front - slice.horizon) * front_inv;
            slice.horizon = front; // it stands on the horizon: the horizon rises
        }
        else
        {
            slice.floating =
                floating ? float2(min(slice.floating.x, low), max(slice.floating.y, front)) : float2(low, front);
        }
        rc_land(slice, front_inv);
        if (!any(covered > 0.0))
        {
            continue;
        }
        const float4 radiance = march.exact ? load_texture(sources.radiance, texel)
                                            : sample_texture_level(sources.radiance, kSamplerLinearClamp, uv, march.mip);
        if (radiance.a <= 1e-3)
        {
            continue; // the sky: no surface to take light from
        }
        const float4 surface = march.exact ? load_texture(sources.normal, texel)
                                           : sample_texture_level(sources.normal, kSamplerLinearClamp, uv, march.mip);
        const float facing = saturate(dot(surface.xyz, to_front) * -front_inv * 8.0);
        const float3 light = radiance.rgb / radiance.a * lerp(saturate(surface.a / radiance.a), 1.0, facing);
        for (uint e = 0; e < kRcElevations; ++e)
        {
            slice.light[e] += light * covered[e];
        }
    }
}

// The cascade above, round a probe: its four probes there, how much each counts, and where and on what each stands.
public struct RcUpper
{
    public uint texture;   // the cascade above, merged (sampled)
    public bool wide;      // it stores every azimuth, not the mean of each two
    public uint2 probes;   // its probes each way
    public int2 first;     // the first of the four
    public float4 weights; // of the four: (0, 0), (1, 0), (0, 1), (1, 1); they sum to at most 1: the less, the less
                           // there is to read, and the more what's past the probe's own interval is taken as open
    public float4 behind;  // of each, how far this probe stands in front of the surface that one stands on: 0
                           // on it or behind it, 1 well in front
    public float3 normals[4];
    public float3 offsets[4]; // from this probe to each
};

// Finds the four probes of the cascade above round a probe, each weighed by nearness, by how near it stands to the
// probe's own plane (and hardly at all if it's behind that plane: past the probe's own surface) and by how alike
// the surfaces they stand on face. A probe far off that plane, or on a surface turned another way, sees another
// sky: the floor at a wall's foot doesn't speak for the wall's top, nor the wall's face beside it. Where none are
// alike (a pillar too thin to have a probe of its own up here), any that stand near enough count instead, by how
// far away they are: what they saw is set where this probe would see it (rc_merge), so they speak for it roughly.
// With none near there's nothing to read.
public RcUpper rc_upper(RcRays rays, RcProbe probe, RcSources sources, uint texture, bool wide, uint2 probes,
                        uint spacing, float tolerance)
{
    RcUpper upper;
    upper.texture = texture;
    upper.wide = wide;
    upper.probes = probes;
    const float2 grid = (probe.at - 0.5 - float(spacing / 2)) / float(spacing);
    const int2 last = int2(probes) - 1;
    upper.first = clamp(int2(floor(grid)), int2(0), max(last - 1, int2(0)));
    const float2 part = saturate(grid - float2(upper.first));
    // How far apart the cascade above's probes stand at the probe's depth, in meters, times the tolerance.
    const float reach = length(rays.dx) * length(probe.offset) / max(length(rc_ray(rays, probe.at)), 1e-9) *
                        float(spacing) * tolerance;
    float4 alike = float4(0.0);
    float4 fallback = float4(0.0);
    for (uint corner = 0; corner < 4; ++corner)
    {
        const int2 offset = int2(corner & 1u, corner >> 1);
        const uint2 pixel = rc_probe_pixel(uint2(min(upper.first + offset, last)), spacing, sources.size);
        const float2 share = lerp(1.0 - part, part, float2(offset));
        const float depth = load_texture(sources.depth, int2(pixel)).r;
        upper.normals[corner] = load_texture(sources.normal, int2(pixel)).xyz;
        upper.offsets[corner] = float3(0.0);
        upper.behind[corner] = 0.0;
        if (depth > 0.0)
        {
            const float3 there = rc_ray(rays, float2(pixel) + 0.5) / depth - probe.offset;
            upper.offsets[corner] = there;
            // Off the probe's plane, in reaches: behind it counts four times as far.
            float off_plane = dot(probe.normal, there) / max(reach, 1e-6);
            off_plane *= off_plane < 0.0 ? 4.0 : 1.0;
            const float near_plane = share.x * share.y * exp(-off_plane * off_plane);
            const float turned = saturate(dot(probe.normal, upper.normals[corner]));
            alike[corner] = near_plane * turned * turned * turned * turned;
            // How far in front of that probe's surface: fully, by a seventh of the way to it.
            const float away = length(there);
            upper.behind[corner] = saturate((-dot(upper.normals[corner], there) / max(away, 1e-6) - 0.05) * 10.0);
            const float far = away / max(reach * 3.0, 1e-6);
            fallback[corner] = share.x * share.y * exp(-far * far);
        }
    }
    // The shares sum to 1. Those alike count in full once they hold a tenth of that; as they fall short, the
    // others take their place, and as those fall short too, less and less is read at all.
    const float alike_total = dot(alike, float4(1.0));
    const float fallback_total = dot(fallback, float4(1.0));
    const float alike_trust = saturate(alike_total * 10.0);
    const float fallback_trust = (1.0 - alike_trust) * saturate(fallback_total * 10.0);
    upper.weights = alike * (alike_trust / max(alike_total, 1e-6)) + fallback * (fallback_trust / max(fallback_total, 1e-6));
    return upper;
}

// Merges a slice with what the cascade above holds for it (its stored azimuth `azimuth`): what's hidden is what
// either hides, and of the cascade above's light, what this slice doesn't already hide. The cascade above's light
// lies in what it covers above its own probe's tangent plane (the top of this file), so that's where it's looked for.
public void rc_merge(inout RcSlice slice, RcUpper upper, RcProbe probe, float3 tangent, uint azimuth)
{
    const float trust = dot(upper.weights, float4(1.0));
    if (trust <= 0.0)
    {
        return;
    }
    const bool floating = rc_has_floating(slice);
    // What isn't read stays as this slice has it.
    float horizon = (1.0 - trust) * slice.horizon;
    float risen_by = (1.0 - trust) * slice.risen;
    float floating_length = 0.0;
    float floating_moment = 0.0;
    float3 light[kRcElevations];
    for (uint e = 0; e < kRcElevations; ++e)
    {
        light[e] = float3(0.0);
    }
    const int2 last = int2(upper.probes) - 1;
    for (uint corner = 0; corner < 4; ++corner)
    {
        const float weight = upper.weights[corner];
        if (weight <= 0.0)
        {
            continue;
        }
        const int2 at = min(upper.first + int2(corner & 1u, corner >> 1), last);
        float4 texels[kRcElevations];
        for (uint e = 0; e < kRcElevations; ++e)
        {
            const int down = int(e * upper.probes.y) + at.y;
            if (upper.wide)
            {
                // Every azimuth of the cascade above is stored: the mean of the two that are this slice's.
                texels[e] = 0.5 * (load_texture(upper.texture, int2(int(2 * azimuth * upper.probes.x) + at.x, down)) +
                                   load_texture(upper.texture,
                                                int2(int((2 * azimuth + 1) * upper.probes.x) + at.x, down)));
            }
            else
            {
                texels[e] = load_texture(upper.texture, int2(int(azimuth * upper.probes.x) + at.x, down));
            }
        }
        const float above_horizon = texels[0].a;
        const float above_length = texels[1].a;
        float2 above_floating = float2(0.0);
        if (above_length > 1e-5)
        {
            const float middle = texels[2].a / above_length;
            above_floating = float2(middle - above_length * 0.5, middle + above_length * 0.5);
        }
        // Where the cascade above's light lies: under its horizon down to its own ground, and in its floating piece.
        const float ground = rc_tangent_cos(upper.normals[corner], probe.to_camera, tangent);
        const float floating_from = max(above_floating.x, above_horizon);
        const float floating_to = max(above_floating.y, floating_from);
        const float4 under = rc_share(ground, above_horizon);
        const float4 lit = under + rc_share(floating_from, floating_to);

        // What the cascade above hides, as this probe sees it. Its horizon rose to something a known way off:
        // from here that thing stands elsewhere in the slice (nearer a wall, the wall stands taller).
        const float rise = above_horizon - ground;
        const float above_nearness = rise > 1e-3 ? texels[3].a / rise : 0.0;
        float risen = above_horizon;
        float risen_nearness = above_nearness;
        if (above_nearness > 1e-4)
        {
            const float3 way = probe.to_camera * above_horizon +
                               tangent * sqrt(max(1.0 - above_horizon * above_horizon, 0.0));
            const float3 to_it = upper.offsets[corner] + way / above_nearness;
            risen_nearness = rsqrt(max(dot(to_it, to_it), 1e-12));
            risen = dot(to_it, probe.to_camera) * risen_nearness;
        }

        // Its light, as this probe sees it. A bin of the cascade above holds light at one strength over what's lit
        // in it. What its horizon rose over moves with it: from its ground to its horizon there, from the same
        // ground to where the horizon stands here, stretched alike in between. So each bin's light is laid where
        // this probe sees it, at the strength it had (nearer a lit thing, it spans more and gives more), and
        // what of that this slice already hides is left out: a light on the far side of a wall, or behind this
        // probe's own surface, doesn't reach it. Its floating piece's light stays where it was.
        const float stretch = clamp((risen - ground) / max(rise, 1e-3), 0.0, 4.0);
        for (uint e = 0; e < kRcElevations; ++e)
        {
            if (lit[e] <= 1e-5)
            {
                continue;
            }
            const float low = kRcBinLow[e];
            const float under_from = ground + (max(low, ground) - ground) * stretch;
            const float under_to = ground + (min(low + kRcBinWidth, above_horizon) - ground) * stretch;
            const float apart_from = max(max(low, floating_from), slice.horizon);
            const float apart_to = min(low + kRcBinWidth, floating_to);
            float4 seen = rc_share(max(under_from, slice.horizon), under_to) + rc_share(apart_from, apart_to);
            if (floating)
            {
                seen -= rc_share(max(max(under_from, slice.horizon), slice.floating.x), min(under_to, slice.floating.y)) +
                        rc_share(max(apart_from, slice.floating.x), min(apart_to, slice.floating.y));
            }
            const float3 strength = texels[e].rgb * (weight / lit[e]);
            for (uint to = 0; to < kRcElevations; ++to)
            {
                light[to] += strength * max(seen[to], 0.0);
            }
        }

        // Below its own ground is the plane of the surface its probe stands on: that hides something from this
        // probe only so far as this probe is in front of it (the floor, for a wall's face), not if it's on it or
        // past its edge (a wall's face, for the wall's top). Else what it hides for sure is what rose above that:
        // standing on this slice's horizon it raises it; the wider the gap between, the more of it floats instead.
        const float behind = upper.behind[corner];
        const float stands = max(behind, saturate(1.0 - (ground - slice.horizon - kRcGap) / kRcGap));
        const float raised = max(max(slice.horizon, risen), lerp(slice.horizon, max(ground, slice.horizon), behind));
        const float merged = lerp(slice.horizon, raised, stands);
        horizon += weight * merged;
        // What raised it: the thing the cascade above rose to, if that's what stands highest; its probe's own
        // ground is no thing at a distance.
        risen_by += weight * (slice.risen + (merged - slice.horizon) * (risen >= raised ? risen_nearness : 0.0));
        const float piece_length = floating_to - floating_from;
        const float apart_length = (1.0 - stands) * max(risen - ground, 0.0);
        floating_length += weight * (piece_length + apart_length);
        floating_moment += weight * (piece_length * (floating_from + floating_to) * 0.5 +
                                     apart_length * (ground + max(risen, ground)) * 0.5);
    }
    slice.horizon = horizon;
    slice.risen = risen_by;
    if (floating_length > 1e-5)
    {
        const float middle = floating_moment / floating_length;
        const float2 above = float2(middle - floating_length * 0.5, middle + floating_length * 0.5);
        slice.floating = floating ? float2(min(slice.floating.x, above.x), max(slice.floating.y, above.y)) : above;
    }
    rc_land(slice, 0.0); // how near a piece that lands here is isn't known
    for (uint e = 0; e < kRcElevations; ++e)
    {
        slice.light[e] += light[e];
    }
}

// What a slice stores in a bin's texel (the top of this file): a mean of these is a mean of slices.
public float4 rc_stored(RcSlice slice, uint bin)
{
    const float length = rc_has_floating(slice) ? slice.floating.y - slice.floating.x : 0.0;
    float extra = slice.risen;
    if (bin == 0)
    {
        extra = slice.horizon;
    }
    else if (bin == 1)
    {
        extra = length;
    }
    else if (bin == 2)
    {
        extra = length * (slice.floating.x + slice.floating.y) * 0.5;
    }
    return float4(slice.light[bin], extra);
}
