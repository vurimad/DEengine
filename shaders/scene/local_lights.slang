// Local lights (docs/design.md, Local lights): point lights and spots, as many as reach what's in view. The view is
// cut into clusters (a grid across the picture, and slices that thicken with distance), and scene.light_cull writes,
// for each, a bit per light whose sphere reaches it. A lit point looks its cluster up and adds the lights whose bits
// are set: a few, wherever it is, however many the frame holds.
//
// Anything that lights a surface asks here: the deferred pass (scene.deferred_light), what's see-through
// (scene.transparent), water (scene.water). The deferred pass through the lights' shadows (below).

import common.bindless;
import scene.brdf;
import scene.scene_common;

// A light never counts as nearer than this: its source isn't a point, and a point's light has no top.
public static const float kLightNearestM = 0.2;

// The cluster a point (relative to the render origin) is in. Across the picture by where it projects without TAA's
// offset (the grid keeps still); in depth by its distance along the view, slices a fixed ratio thicker each.
public uint light_cluster(SceneFrame* frame, float3 position)
{
    const uint3 grid = frame.light_grid.xyz;
    const float4 clip = mul_rows(frame.unjittered_view_proj, float4(position, 1.0));
    const float2 across = saturate(clip.xy / max(clip.w, 1e-6) * 0.5 + 0.5);
    const float depth = dot(position - frame.camera_position.xyz, frame.camera_forward.xyz);
    const uint x = min(uint(across.x * float(grid.x)), grid.x - 1);
    const uint y = min(uint(across.y * float(grid.y)), grid.y - 1);
    const float slice = log2(max(depth, 1e-3)) * frame.light_slices.x + frame.light_slices.y;
    const uint z = uint(clamp(slice, 0.0, float(grid.z - 1)));
    return (z * grid.y + y) * grid.x + x;
}

// What a light sends to a point, before any surface: its colour at that distance and angle, and the way to it. Nothing
// past its radius.
public float3 light_reaching(LocalLight light, float3 position, out float3 toward)
{
    const float3 offset = light.position_radius.xyz - position;
    const float distance2 = dot(offset, offset);
    const float radius2 = light.position_radius.w * light.position_radius.w;
    toward = offset * rsqrt(max(distance2, 1e-8));
    if (distance2 >= radius2)
    {
        return float3(0.0);
    }
    // By the square of the distance, brought smoothly to nothing at its radius (the window of Karis, "Real Shading in
    // Unreal Engine 4", SIGGRAPH 2013) so a light ends where it's culled.
    const float window = saturate(1.0 - (distance2 * distance2) / (radius2 * radius2));
    const float falloff = window * window / max(distance2, kLightNearestM * kLightNearestM);
    // A spot: all of it inside where its edge starts to fade, none past its cone. A point light's numbers give 1.
    const float cone = saturate((dot(-toward, light.direction_cone.xyz) - light.direction_cone.w) * light.color_cone.w);
    return light.color_cone.rgb * (falloff * cone * cone);
}

// The local shadow atlas (render::LocalLights picks the lights that have tiles in it, and how large): a tile a face,
// where the frame's table says (local_shadow_tile); a point light's six faces one after another (+x, -x, +y, -y, +z,
// -z), a spot's one. Depth is reverse Z (the faces' near over the distance along the face's axis).
//
// A shadow is soft by what casts it: a light has a size (a flame's, a lamp's), so the edge of a shadow is as wide
// as the source looks from the lit point past the thing in the way: sharp where a thing stands on what it shadows,
// wider the farther the shadow falls from it (Fernando, "Percentage-Closer Soft Shadows", 2005). First a few looks
// round the way to the light for what's in the way and how far off it is; with nothing, the point is lit and that's
// all; else the edge's width from that, and that many more looks across it, turned by a pixel's own turn so the
// few of them show as grain, not bands (and TAA smooths the grain).
//
// Every look is a way out from the light, not a place on a tile: the face that way lies through is found for each,
// so an edge runs across a cube's faces with no seam (tried: every look through the one face the point's own way
// lies through, the faces seeing a little past their edges: a tenth cheaper, and a wide edge is cut off square
// along the cube's seams, which the user saw at once), and what a face holds is turned into a distance along that
// way, the same whatever face it's on and whatever its tile's size. What's lit is asked of the surface's own plane
// along each way (not of the one point), so a surface the light grazes doesn't shadow itself and needs little room.

// A face's near plane (kLocalShadowNearM in engine/render/scene/local_lights.h).
static const float kLocalShadowNearM = 0.05;
// The radius of a light's source: a flame's, a lamp's. Till a light says its own.
public static const float kLocalLightSourceM = 0.035;
// The looks: for what's in the way, and across the edge. Each set spread evenly over a disk (a sunflower's seeds).
static const uint kLocalShadowBlockerLooks = 4;
static const uint kLocalShadowEdgeLooks = 8;
static const float2 kLocalShadowBlockerDisk[4] = { float2(0.35355, 0.00000), float2(-0.45154, 0.41365), float2(0.06912, -0.78754), float2(0.56914, 0.74235) };
static const float2 kLocalShadowEdgeDisk[8] = { float2(0.25000, 0.00000), float2(-0.31929, 0.29250), float2(0.04887, -0.55688), float2(0.40244, 0.52492), float2(-0.73854, -0.13064), float2(0.69960, -0.44503), float2(-0.23400, 0.87048), float2(-0.44627, -0.85927) };
// The widest an edge is looked across, and what's in the way looked for, as the tangent of the angle from the light.
static const float kLocalShadowWidestTan = 0.14;

// How far the light saw along a way out from it (not unit: as long as `reach`), in meters: what its face that way
// holds. Nothing: 1e30. A point light's faces are a cube's, worked out here as render::add_shadow_faces lays them
// (no matrix to read: the way's largest part is the face's depth, the other two across it); a spot's one by its
// matrix.
float local_shadow_seen(SceneFrame* frame, LocalLight light, float3 way, float reach, uint atlas)
{
    uint face = uint(light.shadow.x);
    float depth;
    float2 across;
    if (light.shadow.y > 1.5)
    {
        const float3 a = abs(way);
        if (a.x >= a.y && a.x >= a.z)
        {
            depth = a.x;
            across = float2(way.x > 0.0 ? way.z : -way.z, way.y);
            face += way.x > 0.0 ? 0u : 1u;
        }
        else if (a.y >= a.z)
        {
            depth = a.y;
            across = float2(way.y > 0.0 ? way.x : -way.x, way.z);
            face += way.y > 0.0 ? 2u : 3u;
        }
        else
        {
            depth = a.z;
            across = float2(way.z > 0.0 ? -way.x : way.x, way.y);
            face += way.z > 0.0 ? 4u : 5u;
        }
        across /= depth * light.shadow.z;
    }
    else
    {
        const float4 clip = local_shadow_clip(frame, face, light.position_radius.xyz + way);
        if (clip.w <= 1e-3)
        {
            return 1e30; // behind a spot's face: its cone leaves that dark already
        }
        depth = clip.w;
        across = clip.xy / clip.w;
        if (any(abs(across) > 1.0))
        {
            return 1e30; // outside a spot's face
        }
    }
    const float3 tile = local_shadow_tile(frame, face);
    const float2 texel = clamp(float2(across.x * 0.5 + 0.5, 0.5 - across.y * 0.5) * tile.z, 0.5, tile.z - 0.5);
    const float stored = load_texture(atlas, int2(tile.xy + texel)).r;
    // What the face holds is a distance along its axis: along the way, that over the way's share of the axis.
    return stored > 0.0 ? kLocalShadowNearM * reach / (stored * depth) : 1e30;
}

// A pixel's own turn for its shadows' looks (Jimenez's interleaved gradient noise, and TAA's sample turns it on
// so the grain is smoothed over frames): its cosine and sine.
public float2 local_shadow_turn(SceneFrame* frame, float2 pixel)
{
    const float turn =
        (frac(52.9829189 * frac(dot(pixel, float2(0.06711056, 0.00583715)))) + frame.taa_params.y * 0.61803399) * 6.2831853;
    return float2(cos(turn), sin(turn));
}

// How much of a light reaches a point past what's between them, 0 to 1 (the top of this section). 1 for a light
// with no tiles, or with no atlas. `turn`: local_shadow_turn.
public float local_shadow(SceneFrame* frame, LocalLight light, float3 position, float3 normal, uint atlas, float2 turn)
{
    if (atlas == 0 || light.shadow.x < 0.0)
    {
        return 1.0;
    }
    const float3 from_light = position - light.position_radius.xyz;
    const float distance_m = length(from_light);
    if (distance_m <= kLocalShadowNearM)
    {
        return 1.0; // nearer the light than its faces see
    }
    const float3 way = from_light / distance_m;
    // A texel of its tiles, as the tangent of the angle it spans, and in meters out here.
    const float texel_tan = light.shadow.z * 2.0 / local_shadow_tile(frame, uint(light.shadow.x)).z;
    const float texel_m = distance_m * texel_tan;
    // The surface's plane, a little off the surface: each look is asked how far along it the plane is.
    const float plane = dot(from_light, normal) + texel_m * 0.75 + 0.003;
    const float room_m = texel_m * 1.25 + 0.004;
    // Two ways across the way to the light, turned by the pixel's turn.
    const float3 helper = abs(way.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 flat_side = normalize(cross(helper, way));
    const float3 flat_up = cross(way, flat_side);
    const float3 side = flat_side * turn.x + flat_up * turn.y;
    const float3 up = flat_up * turn.x - flat_side * turn.y;

    // What's in the way, round the way to the light: as wide as a thing a quarter of the way out could hide the
    // source from, at least a few texels.
    const float search_tan = clamp(3.0 * kLocalLightSourceM / distance_m, texel_tan * 2.5, kLocalShadowWidestTan);
    float blockers = 0.0;
    float blocker_m = 0.0;
    for (uint i = 0; i < kLocalShadowBlockerLooks; ++i)
    {
        const float2 at = kLocalShadowBlockerDisk[i] * search_tan;
        const float3 look = way + side * at.x + up * at.y;
        const float reach = sqrt(1.0 + dot(at, at));
        const float facing = dot(look, normal);
        const float lit_m = facing < -0.02 ? clamp(plane * reach / facing, distance_m * 0.5, distance_m * 2.0) : distance_m;
        const float seen_m = local_shadow_seen(frame, light, look, reach, atlas);
        if (seen_m < lit_m - room_m)
        {
            blockers += 1.0;
            blocker_m += seen_m;
        }
    }
    if (blockers <= 0.0)
    {
        return 1.0;
    }
    blocker_m /= blockers;
    // The edge's width out here is the source's, by how far past the thing in the way the point is over how far
    // the thing is from the light: as a tangent from the light, over the distance. Never narrower than two texels:
    // the looks then blur a tile's own steps, which a narrower edge would show cut out square.
    const float edge_tan = clamp(kLocalLightSourceM * (distance_m - blocker_m) / (max(blocker_m, kLocalShadowNearM) * distance_m),
                                 texel_tan * 2.0, kLocalShadowWidestTan);
    if (blockers >= float(kLocalShadowBlockerLooks) && edge_tan <= search_tan)
    {
        return 1.0 - light.shadow.w; // every look round was in shadow, and the edge is no wider than they looked
    }
    float lit = 0.0;
    for (uint i = 0; i < kLocalShadowEdgeLooks; ++i)
    {
        const float2 at = kLocalShadowEdgeDisk[i] * edge_tan;
        const float3 look = way + side * at.x + up * at.y;
        const float reach = sqrt(1.0 + dot(at, at));
        const float facing = dot(look, normal);
        const float lit_m = facing < -0.02 ? clamp(plane * reach / facing, distance_m * 0.5, distance_m * 2.0) : distance_m;
        lit += local_shadow_seen(frame, light, look, reach, atlas) >= lit_m - room_m ? 1.0 : 0.0;
    }
    return lerp(1.0, lit / float(kLocalShadowEdgeLooks), light.shadow.w); // as much of its shadow as shows
}

// What a surface is, to a light.
public struct LitSurface
{
    public float3 position; // relative to the render origin
    public float3 normal;
    public float3 to_camera; // unit
    public float3 albedo;    // what it scatters back: none of it for a metal
    public float3 f0;
    public float a;              // roughness squared
    public float specular_share; // the lighting group's
    public float translucency;   // wraps the light round it
};

// What they add to it: what it scatters and what it mirrors, apart (something see-through keeps all of the second and
// only its opacity's share of the first).
public struct LocalLighting
{
    public float3 diffuse;
    public float3 specular;
};

// The light the local lights of a surface's cluster add to it, as the sun's is worked out; through their shadows
// where they have them and shadow_atlas is given (the local shadow atlas, sampled; 0: none). `pixel`: where on the
// picture the surface is, for the shadows' looks.
public LocalLighting local_lights(SceneFrame* frame, LitSurface surface, uint shadow_atlas = 0, float2 pixel = float2(0.0))
{
    LocalLighting sum;
    sum.diffuse = float3(0.0);
    sum.specular = float3(0.0);
    const uint words = frame.light_grid.w;
    if (words == 0)
    {
        return sum;
    }
    const uint first = light_cluster(frame, surface.position) * words;
    const float2 turn = shadow_atlas != 0 ? local_shadow_turn(frame, pixel) : float2(1.0, 0.0);
    const float n_dot_v = max(dot(surface.normal, surface.to_camera), 1e-4);
    const float wrap = surface.translucency * 0.5;
    for (uint word = 0; word < words; ++word)
    {
        uint mask = frame.light_clusters[first + word];
        while (mask != 0)
        {
            const uint bit = firstbitlow(mask);
            mask &= mask - 1;
            float3 l;
            const LocalLight light = frame.lights[word * 32 + bit];
            float3 reaching = light_reaching(light, surface.position, l);
            if (reaching.r + reaching.g + reaching.b <= 0.0)
            {
                continue; // in its cluster, out of its reach or its cone
            }
            // Its shadow, where it lights enough for one to show: the last of a light's reach is too faint to tell.
            const float matters = saturate((reaching.r + reaching.g + reaching.b) * 500.0 - 0.5);
            if (matters > 0.0 && dot(surface.normal, l) > -0.5)
            {
                reaching *= lerp(1.0, local_shadow(frame, light, surface.position, surface.normal, shadow_atlas, turn), matters);
            }
            const float n_dot_l_raw = dot(surface.normal, l);
            const float n_dot_l = saturate(n_dot_l_raw);
            const float diffuse_term = saturate((n_dot_l_raw + wrap) / (1.0 + wrap)) / (1.0 + wrap);
            const float3 half_vector = l + surface.to_camera;
            const float3 h = dot(half_vector, half_vector) > 1e-8 ? normalize(half_vector) : surface.normal;
            const float3 fresnel = f_schlick(surface.f0, saturate(dot(surface.to_camera, h)));
            sum.diffuse += reaching * surface.albedo * (1.0 - fresnel) * (diffuse_term / kPi);
            sum.specular += reaching * (d_ggx(saturate(dot(surface.normal, h)), surface.a) *
                                        v_smith(n_dot_v, n_dot_l, surface.a) * fresnel * n_dot_l * surface.specular_share);
        }
    }
    return sum;
}
