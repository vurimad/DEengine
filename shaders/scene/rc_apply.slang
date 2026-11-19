// The radiance cascades' light on each of the screen's pixels (docs/design.md, Global illumination: radiance
// cascades): what scene.rc_gather gathered over the GI picture (the light's moments over directions), taken from the
// four of its pixels round this one and turned into the light on this pixel's own surface.
//
// The GI picture is smaller than the screen, so its four pixels round this one may stand on other surfaces. Each
// counts by nearness, by how near this pixel is to the plane of the surface it stands on (hardly at all if this
// pixel is behind that plane: a wall's face, for a pixel of the wall's top) and by how alike this pixel's normal and
// that surface's are. Where none count (a thing thinner than the GI picture's pixels), any that stand close by do
// instead; where none stand close either (a bumpy wall seen edge on: its bumps' normals turn from the surface the
// GI picture's pixels stand on, and those pixels are meters apart along it), whichever of the four stand on a
// surface at all, by nearness on the screen alone: their light is a truer guess than the flat ambient, which in a
// dark hall is the open sky's blue in specks. With none at all (the sky all round), the flat ambient.
//
// The light on this pixel, with its own normal n (the material's normal map and all), from what one of those
// gathered. Nothing arrives there from behind the surface it stands on, so for a normal that is that surface's the
// light is exactly moment 1 . n: the integral of the radiance times the cosine. For a normal turned well away from
// it (another surface's, borrowed across a corner) that would count light from behind this pixel as less than
// none, so there it goes over to the moments' first two spherical harmonics, moment 0 / 4 + (moment 1 . n) / 2:
// rougher, but right for a sky the same over a hemisphere whichever way the surface faces.
//
// This is the one pass at the screen's size, so it reads as little as it can: the four depths in one gather, and
// the four pixels' moments, weighed, by two filtered samples a colour (a sample between two texels of a row is
// their mean by any two weights; the two rows' are then added by theirs).
//
// What the surface mirrors (its specular ambient) comes from the same moments: the light arriving along the
// mirrored way r, as the plainest light that has those moments. Over the hemisphere above a surface of normal n,
// light a + b . w has moment 0 = 2 pi a + pi b . n and moment 1 = pi a n + 2 pi / 3 b, so
// a = 2 / pi (moment 0 - 3 / 2 moment 1 . n) and the light along r is a (1 - 3 / 2 n . r) + 3 / (2 pi) moment 1 . r:
// an even light comes out even whichever way r points (the first two spherical harmonics, which take the dark
// under the surface for part of the light, would halve it at a grazing r, where a surface mirrors most). For a
// normal turned away from the surface the moments were gathered on, the spherical harmonics again,
// moment 0 / (4 pi) + 3 / (4 pi) moment 1 . r. A rough surface mirrors a wider lobe round r, and a lobe takes less of
// the light's lean: b . r counts by 1 for a mirror down to 2/3 for the roughest (a cosine lobe's share, which makes
// that case the diffuse light's own formula). It's a broad light, no picture of what's mirrored; what it has
// that the flat sky hadn't is the place's own light: its colour, where it comes from, what hides it.
//
// Added onto the lit picture as its ambient, diffuse and specular (the deferred light leaves both out while the
// cascades are on), times the material's own occlusion. The cascades see what's near on the screen themselves, so
// GTAO isn't applied on top.

import common.bindless;
import scene.brdf;
import scene.rc_common;
import scene.scene_common;
import scene.sky;

struct RcApplyPush
{
    SceneFrame* frame;
    uint gb0;
    uint gb1;
    uint gb2;
    uint scene_depth;        // the screen's depth
    uint depth;              // the GI picture's (scene.rc_pyramid: their first mips are read)
    uint normal;
    uint light;              // the moments (scene.rc_gather)
    uint scale;              // the screen's pixels to one of the GI picture's, each way
    uint2 size;              // the screen's
    uint2 gi_size;           // the GI picture's
    float tolerance;         // of the plane weight, in the GI picture's pixels
    uint flags;              // kMaterialOcclusion, kIndirect, kSpecular
    float reserved_sky;      // (the flat ambient, where it's all there is, counts by scene.sky's open_sky)
    uint reserved;
};

static const uint kMaterialOcclusion = 1; // times the material's own occlusion (off to compare with the path tracer)
static const uint kIndirect = 2;          // only the light gathered, whatever the surface's colour (the indirect view)
static const uint kSpecular = 4;          // what surfaces mirror too (off to compare with the path tracer)

[[vk::push_constant]]
ConstantBuffer<RcApplyPush> push;

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
    const int2 pixel = int2(input.position.xy);
    const float depth = load_texture(push.scene_depth, pixel).r;
    if (depth <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0); // added, alpha is a factor: 1
    }
    const GBufferSample s = unpack_gbuffer(load_texture(push.gb0, pixel), load_texture(push.gb1, pixel).xy,
                                           load_texture(push.gb2, pixel));
    const float3 n = s.normal;

    // The screen's own rays: where this pixel is, and where the GI picture's pixels round it are.
    const RcRays rays = rc_rays(frame, push.size, 1);
    const float3 ray = rc_ray(rays, float2(pixel) + 0.5);
    const float3 here = ray / depth;
    // How far apart the GI picture's pixels stand at this depth, in meters, times the tolerance.
    const float reach =
        max(length(rays.dx) * length(here) / max(length(ray), 1e-9) * float(push.scale) * push.tolerance, 1e-6);

    const float2 grid = (float2(pixel) - float(push.scale / 2)) / float(push.scale);
    const int2 last = int2(push.gi_size) - 1;
    const int2 first = clamp(int2(floor(grid)), int2(0), max(last - 1, int2(0)));
    const float2 part = saturate(grid - float2(first));
    // The four depths: a gather's order is (0, 1), (1, 1), (1, 0), (0, 0).
    const float2 gi_size = float2(push.gi_size);
    const float4 gathered =
        texture_2d(push.depth).GatherRed(sampler_state(kSamplerLinearClamp), (float2(first) + 1.0) / gi_size);
    const float4 depths = float4(gathered.w, gathered.z, gathered.x, gathered.y);
    float4 near = float4(0.0);
    float4 close = float4(0.0);
    float4 any = float4(0.0);
    float4 same = float4(0.0); // how far this pixel's normal is that surface's: 0 turned away, 1 the same
    for (uint corner = 0; corner < 4; ++corner)
    {
        const int2 offset = int2(corner & 1u, corner >> 1);
        const int2 at = min(first + offset, last);
        const float2 share = lerp(1.0 - part, part, float2(offset));
        const float there_depth = depths[corner];
        if (there_depth <= 0.0 || share.x * share.y <= 0.0)
        {
            continue;
        }
        const float3 there_normal = load_texture(push.normal, at).xyz;
        const float alike = saturate(dot(n, there_normal));
        same[corner] = smoothstep(0.0, 0.7, alike);
        const uint2 on_screen = rc_screen_pixel(uint2(at), push.scale, push.size);
        const float3 there = rc_ray(rays, float2(on_screen) + 0.5) / there_depth - here;
        // This pixel off the plane of the surface that one stands on, in reaches: behind it counts four times as far.
        float off_plane = -dot(there_normal, there) / reach;
        off_plane *= off_plane < 0.0 ? 4.0 : 1.0;
        near[corner] = share.x * share.y * exp(-off_plane * off_plane) * alike * alike;
        const float far = length(there) / (reach * 3.0);
        close[corner] = share.x * share.y * exp(-far * far);
        any[corner] = share.x * share.y;
    }
    // The shares sum to 1. Those near count in full once they hold a tenth of that; as they fall short, those close
    // take their place, as those fall short too, any that stand on a surface, and only then the flat ambient.
    const float near_total = dot(near, float4(1.0));
    const float close_total = dot(close, float4(1.0));
    const float any_total = dot(any, float4(1.0));
    const float near_trust = saturate(near_total * 10.0);
    const float close_trust = (1.0 - near_trust) * saturate(close_total * 10.0);
    const float any_trust = (1.0 - near_trust - close_trust) * saturate(any_total * 10.0);
    const float4 weights = near * (near_trust / max(near_total, 1e-6)) + close * (close_trust / max(close_total, 1e-6)) +
                           any * (any_trust / max(any_total, 1e-6));

    // The four pixels' moments, weighed: each row's two by one sample between them, where their weights say.
    const float2 rows = float2(weights[0] + weights[1], weights[2] + weights[3]);
    const float2 across = float2(weights[1], weights[3]) / max(rows, 1e-12);
    const float2 light_size = float2(gi_size.x, gi_size.y * 3.0);
    const float2 x = min(float(first.x) + 0.5 + across, float(last.x) + 0.5) / light_size.x;
    const float2 y = float2(float(first.y), float(min(first.y + 1, last.y))) + 0.5;
    float4 moments[3]; // red's, green's, blue's
    for (uint colour = 0; colour < 3; ++colour)
    {
        const float2 down = (y + float(colour) * gi_size.y) / light_size.y;
        moments[colour] = rows.x * sample_texture_level(push.light, kSamplerLinearClamp, float2(x.x, down.x), 0.0) +
                          rows.y * sample_texture_level(push.light, kSamplerLinearClamp, float2(x.y, down.y), 0.0);
    }
    // Over pi: what a surface of albedo 1 gives back, as scene.sky's hemisphere() is.
    const float3 all = float3(moments[0].x, moments[1].x, moments[2].x);
    const float3 along = float3(dot(moments[0].yzw, n), dot(moments[1].yzw, n), dot(moments[2].yzw, n));
    const float trust = near_trust + close_trust + any_trust;
    const float as_same = dot(same, weights) / max(trust, 1e-6);
    float3 light = max(lerp(all * 0.25 + along * 0.5, along, as_same), 0.0) / kRcPi;
    // The flat ambient where nothing of the GI picture stands in, by how open to the sky the place is.
    const float flat_share =
        trust < 0.999 ? (1.0 - trust) * open_sky(frame, frame.camera_position.xyz + here + n * kOpenSkyAskedM) : 0.0;
    light += hemisphere(frame, n) * flat_share;
    const float occlusion = (push.flags & kMaterialOcclusion) != 0 ? s.occlusion : 1.0;
    light *= occlusion;
    if ((push.flags & kIndirect) != 0)
    {
        return float4(min(light, kHalfMax), 1.0);
    }
    const float3 albedo = s.base_color * (1.0 - s.metalness);
    float3 lit = albedo * light;
    if ((push.flags & kSpecular) != 0)
    {
        // What the surface mirrors: the light along the mirrored way (the top of this file).
        const float3 v = -normalize(ray);
        const float n_dot_v = max(dot(n, v), 1e-4);
        const float3 r = reflect(-v, n);
        const float3 along_mirror = float3(dot(moments[0].yzw, r), dot(moments[1].yzw, r), dot(moments[2].yzw, r));
        const float lean = 1.0 - s.roughness / 3.0;
        const float3 even = (all - 1.5 * along) * (2.0 / kRcPi);
        const float3 as_surface = even * (1.0 - 1.5 * lean * n_dot_v) + along_mirror * (1.5 * lean / kRcPi);
        const float3 as_sphere = (all * 0.25 + along_mirror * (0.75 * lean)) / kRcPi;
        float3 mirrored = max(lerp(as_sphere, as_surface, as_same), 0.0);
        mirrored += hemisphere(frame, r) * flat_share;
        const float3 f0 = lerp(float3(0.04), s.base_color, s.metalness);
        const float specular_share = s.lighting_group == kLightingGroupGround ? kGroundSpecularShare : 1.0;
        lit += env_brdf(f0, s.roughness, n_dot_v) * specular_share * mirrored *
               specular_occlusion(n_dot_v, occlusion, s.roughness);
    }
    return float4(min(lit, kHalfMax), 1.0);
}
