// One path of light, traced back from where it arrives (docs/design.md, Global illumination: the plan from
// 2026-11-14): what the path tracer adds up for a pixel, and what a light box's bake adds up for a voxel. Same rays,
// materials, lights, sky and conventions as the rasterized picture.
//
// From a start, in a direction, to what it meets; at each surface its own glow, and the sun and the local lights by
// rays toward them (a surface they don't reach gets nothing from them: a shadow), each through the same BRDF as the
// deferred light; then on, in a direction drawn by the cosine (light that bounces, scattered by the surface's colour),
// until it leaves into the sky or is ended by chance (Russian roulette, which keeps the mean exact). What leaves into
// the sky brings the sky's light from that way (scene.sky's sky_light), whose sum over an open hemisphere is the flat
// ambient the lit picture uses where no light box holds a point; a ray from the camera into the sky shows the sky as
// drawn.
//
// What it doesn't do yet: light that bounces off a mirror-like surface (bounces are diffuse; a surface's own
// highlight from the sun and the lights is there), normal maps (rays see the vertices' normals), translucency and the
// ground group's specular share, the fog, see-through things. The sun's shadow keeps the environment's shadow colour,
// as the lit picture does.

import common.bindless;
import scene.brdf;
import scene.local_lights;
import scene.scene_common;
import scene.sky;
import scene.trace;

// Off a surface by this much before a ray leaves it, so it doesn't meet the surface it left.
public static const float kPathOffsetM = 1e-3;
public static const float kPathFarM = 100000.0;

// A hash to numbers that look random (Wellons's lowbias32).
public uint path_hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

public float next_unit(inout uint state)
{
    state = path_hash(state);
    return float(state >> 8) * (1.0 / 16777216.0);
}

// A direction about n, drawn with a chance by the cosine.
public float3 cosine_direction(float3 n, float u1, float u2)
{
    const float3 helper = abs(n.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 t = normalize(cross(helper, n));
    const float3 b = cross(n, t);
    const float r = sqrt(u1);
    const float phi = 2.0 * kPi * u2;
    return normalize(t * (r * cos(phi)) + b * (r * sin(phi)) + n * sqrt(max(1.0 - u1, 0.0)));
}

// A direction over the whole sphere, every one as likely.
public float3 sphere_direction(float u1, float u2)
{
    const float z = 1.0 - 2.0 * u1;
    const float r = sqrt(max(1.0 - z * z, 0.0));
    const float phi = 2.0 * kPi * u2;
    return float3(r * cos(phi), r * sin(phi), z);
}

// What light arriving from l sends toward v off the surface, as the deferred light works out the sun's and the
// local lights' (GGX and Lambert, the diffuse taking what the Fresnel leaves).
float3 reflected(TraceSurface s, float3 v, float3 l, float3 light)
{
    const float3 n = s.normal;
    const float n_dot_l = dot(n, l);
    if (n_dot_l <= 0.0)
    {
        return float3(0.0);
    }
    const float3 half_vector = l + v;
    const float3 h = dot(half_vector, half_vector) > 1e-8 ? normalize(half_vector) : n;
    const float a = max(s.roughness * s.roughness, 2e-3);
    const float3 f0 = lerp(float3(0.04), s.base_color, s.metalness);
    const float3 albedo = s.base_color * (1.0 - s.metalness);
    const float3 fresnel = f_schlick(f0, saturate(dot(v, h)));
    const float3 specular = d_ggx(saturate(dot(n, h)), a) * v_smith(max(dot(n, v), 1e-4), n_dot_l, a) * fresnel * n_dot_l;
    const float3 diffuse = albedo * (1.0 - fresnel) * (n_dot_l / kPi);
    return light * (diffuse + specular);
}

// What one path brings, and whether its first ray met the back of something solid (the start is inside it).
public struct PathLight
{
    public float3 light;
    public bool back;
};

// The light arriving at a start from a direction, by one path. from_camera: the first ray shows the sky as drawn and
// goes on past the backs of one-sided surfaces, as the picture does; else the sky is sky_light and a back ends it.
public PathLight trace_path(SceneFrame* frame, float3 start, float3 way, inout uint seed, uint max_bounces,
                            uint light_count, bool from_camera)
{
    PathLight result;
    result.light = float3(0.0);
    result.back = false;
    float3 origin = start;
    float3 direction = way;
    float3 throughput = float3(1.0);
    for (uint bounce = 0; bounce <= max_bounces; ++bounce)
    {
        TraceHit hit = trace_nearest(frame.trace, origin, direction, 0.0, kPathFarM);
        for (uint skip = 0; from_camera && bounce == 0 && skip < 4 && hit.hit && !hit.front; ++skip)
        {
            const TraceSurface back = trace_surface(frame.trace, hit, origin, direction, 0.0);
            if ((back.flags & kMaterialDoubleSided) != 0)
            {
                break;
            }
            // On from just past it: what it meets next is placed from there, not from the camera.
            origin = back.position + direction * kPathOffsetM;
            hit = trace_nearest(frame.trace, origin, direction, 0.0, kPathFarM);
        }
        if (!hit.hit)
        {
            result.light += throughput * (bounce == 0 && from_camera ? sky(frame, direction) : sky_light(frame, direction));
            break;
        }
        const TraceSurface s = trace_surface(frame.trace, hit, origin, direction, 0.0);
        if (!hit.front && (s.flags & kMaterialDoubleSided) == 0)
        {
            result.back = result.back || bounce == 0;
            break; // the back of something solid: no light comes out of it
        }
        const float3 v = -direction;
        const float3 leave = s.position + s.normal * kPathOffsetM;
        result.light += throughput * s.emissive;
        // The sun, through a ray toward it; what it doesn't reach keeps the shadow's colour, as the lit picture has.
        const float3 sun_way = frame.sun_direction.xyz;
        if (dot(s.normal, sun_way) > 0.0)
        {
            const bool hidden = trace_blocked(frame.trace, leave, sun_way, 0.0, kPathFarM);
            const float3 sun = frame.sun_color.rgb * (hidden ? frame.shadow_color.rgb : float3(1.0));
            result.light += throughput * reflected(s, v, sun_way, sun);
        }
        // The local lights, each through a ray toward it.
        for (uint i = 0; i < light_count; ++i)
        {
            const LocalLight light = frame.lights[i];
            float3 toward;
            const float3 reaching = light_reaching(light, s.position, toward);
            if (reaching.r + reaching.g + reaching.b <= 0.0 || dot(s.normal, toward) <= 0.0)
            {
                continue;
            }
            // A light has a size (kLocalLightSourceM: what makes its shadows' edges soft): the ray goes to a place on
            // its source, a disk facing this point, drawn evenly.
            const float3 across = normalize(cross(abs(toward.y) < 0.9 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0), toward));
            const float turn = next_unit(seed) * 6.2831853;
            const float out_m = sqrt(next_unit(seed)) * kLocalLightSourceM;
            const float3 on_source =
                light.position_radius.xyz + (across * cos(turn) + cross(toward, across) * sin(turn)) * out_m;
            const float distance = length(on_source - leave);
            const float3 to_source = (on_source - leave) / max(distance, 1e-6);
            if (!trace_blocked(frame.trace, leave, to_source, 0.0, max(distance - kPathOffsetM, 0.0)))
            {
                result.light += throughput * reflected(s, v, toward, reaching);
            }
        }
        if (bounce == max_bounces)
        {
            break;
        }
        // On, scattered by its colour (a cosine-drawn direction: the cosine and the 1/pi cancel the chance).
        throughput *= s.base_color * (1.0 - s.metalness);
        if (bounce >= 3)
        {
            const float keep = clamp(max(throughput.r, max(throughput.g, throughput.b)), 0.05, 0.95);
            if (next_unit(seed) >= keep)
            {
                break;
            }
            throughput /= keep;
        }
        if (max(throughput.r, max(throughput.g, throughput.b)) <= 0.0)
        {
            break;
        }
        origin = leave;
        direction = cosine_direction(s.normal, next_unit(seed), next_unit(seed));
    }
    result.light = select(isfinite(result.light), max(result.light, 0.0), 0.0);
    return result;
}
