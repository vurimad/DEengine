// The wind (docs/design.md, Vegetation; Sky, atmosphere & weather): one field over the world, from the environment
// (render::SceneWind). A steady push one way; gusts, patches of faster air that travel with it; and turbulence, smaller
// swirls that turn it off its way. Grass and plants read it here, so they move together.
//
// Times wrap at kWindPeriodS and every rhythm below repeats within it (a whole number of beats per period), so nothing
// jumps when they wrap. The gust field repeats every kWindCells patches, and the renderer wraps its offset to that, in
// double precision, so it stays put in the world however long the world has run and wherever the render origin is.
// Each mover also asks for last frame's wind, for its motion vector: TAA would smear motion it isn't told of.

import scene.scene_common;

public static const float kWindPeriodS = 256.0;
static const int kWindCells = 64;
static const float kTau = 6.2831853;

// Beats per second for each rhythm: whole numbers of beats per kWindPeriodS.
static const float kSwirlHz = 8.0 / 256.0;    // turbulence turning about, once in 32 s
public static const float kBobHz = 320.0 / 256.0;     // a grass blade's quick sway, 1.25 a second
public static const float kRockHz = 104.0 / 256.0;    // a plant rocking as a whole, about 0.4 a second
public static const float kFlutterHz = 640.0 / 256.0; // leaves and twigs, 2.5 a second

uint wind_hash(int2 cell, int period)
{
    const uint2 c = uint2(((cell % period) + period) % period);
    uint h = c.x * 0x8da6b343u ^ c.y * 0xd8163841u;
    h ^= h >> 13u;
    h *= 0x5bd1e995u;
    h ^= h >> 15u;
    return h;
}

// Smooth noise, 0 to 1, repeating every `period` cells.
float wind_noise(float2 at, int period)
{
    const float2 cell = floor(at);
    const float2 f = at - cell;
    const float2 s = f * f * (3.0 - 2.0 * f);
    const int2 c = int2(cell);
    const float a = float(wind_hash(c, period) & 0xffffu);
    const float b = float(wind_hash(c + int2(1, 0), period) & 0xffffu);
    const float d = float(wind_hash(c + int2(0, 1), period) & 0xffffu);
    const float e = float(wind_hash(c + int2(1, 1), period) & 0xffffu);
    return lerp(lerp(a, b, s.x), lerp(d, e, s.x), s.y) / 65535.0;
}

// The wind time, now or a frame ago, in [0, kWindPeriodS).
public float wind_time(SceneFrame* frame, bool previous)
{
    return previous ? frame.wind_field.w : frame.wind_field.z;
}

// The wind at a point on the ground (render-origin space, x and z), now or a frame ago: which way it pushes and how
// hard, in m/s. Zero in still air.
public float2 wind_at(SceneFrame* frame, float2 xz, bool previous)
{
    const float4 wind = frame.wind;
    if (wind.z + wind.w <= 0.0)
    {
        return float2(0.0);
    }
    const float2 offset = previous ? frame.wind_offset.zw : frame.wind_offset.xy;
    const float2 at = (xz + offset) / frame.wind_field.x;
    // Gusts: mostly lull, and a gust stands out of it.
    const float g = smoothstep(0.35, 0.9, wind_noise(at, kWindCells));
    const float speed = wind.z + wind.w * g;
    // Turbulence: swirls a third the size, their pattern turning about over time, bending the wind off its way and
    // changing its strength a little.
    const float t = wind_time(frame, previous) * kSwirlHz * kTau;
    const float swirl = wind_noise(at * 3.0 + 5.0 * float2(cos(t), sin(t)), kWindCells * 3) * 2.0 - 1.0;
    const float turbulence = frame.wind_field.y;
    const float angle = swirl * turbulence * 0.8;
    const float2 way = float2(wind.x * cos(angle) - wind.y * sin(angle), wind.x * sin(angle) + wind.y * cos(angle));
    return way * speed * (1.0 + 0.35 * turbulence * swirl);
}

// Ripples: patches of faster air a sixth of a gust across, carried along with the gusts: what a field of grass shows as
// waves sweeping through it. 0 to 1, about 0.5 on average. A whole number of them fits the gust field's repeat.
static const float kRippleScale = 6.0;

public float wind_ripple(SceneFrame* frame, float2 xz, bool previous)
{
    if (frame.wind.z + frame.wind.w <= 0.0)
    {
        return 0.0;
    }
    const float2 offset = previous ? frame.wind_offset.zw : frame.wind_offset.xy;
    return smoothstep(0.2, 0.8, wind_noise((xz + offset) / frame.wind_field.x * kRippleScale, kWindCells * int(kRippleScale)));
}

// A phase for something rooted at a point, the same whatever the render origin: 0 to 1.
public float wind_phase(SceneFrame* frame, float2 xz)
{
    const float2 world = xz + float2(int2(frame.render_origin_steps.xz)) * kMetersPerRenderStep;
    return frac(sin(dot(floor(world * 4.0), float2(12.9898, 78.233))) * 43758.5453);
}

// How far a plant mesh's vertex moves in the wind, now or a frame ago (render-origin space). A plant leans with the wind
// at its root and rocks in it, more the higher up a vertex is (bend_m: how far a point 10 m up moves in a 10 m/s wind);
// its leaves and twigs flutter as far as the vertex color's red says, out of step by its green (flutter_m: at full red,
// in an 8 m/s wind). Flutter fades out past 40 to 90 m from the camera, where it would only shimmer.
public float3 plant_sway(SceneFrame* frame, float3 root, float3 at, float3 normal, float4 color, float bend_m,
                         float flutter_m, bool previous)
{
    const float2 w = wind_at(frame, root.xz, previous);
    const float speed = length(w);
    if (speed < 1e-3)
    {
        return float3(0.0);
    }
    const float3 way = float3(w.x, 0.0, w.y) / speed;
    const float t = wind_time(frame, previous);
    const float phase = wind_phase(frame, root.xz);
    const float height = max(at.y - root.y, 0.0);
    const float rise = (height / 10.0) * (height / 10.0);
    const float lean = bend_m * speed / 10.0;
    const float rock = lean * 0.35 * sin(kTau * (kRockHz * t + phase));
    float3 offset = way * (lean + rock) * rise;

    const float near = 1.0 - smoothstep(40.0, 90.0, length(root - frame.camera_position.xyz));
    const float flutter = flutter_m * color.r * saturate(speed / 8.0) * near * sin(kTau * (kFlutterHz * t + color.g + phase));
    offset += (normal + way * 0.5) * flutter;
    // A plant doesn't stretch: its top comes down as it leans.
    const float sideways = length(offset.xz);
    offset.y -= sideways * sideways / max(2.0 * height, 1.0);
    return offset;
}
