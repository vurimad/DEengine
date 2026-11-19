// The waves that run in to the shore, as a formula of the place and the time: bands of the distance to land, each
// carrying one wave (the surf), and what each does at the waterline (the swash). The water draws them where the surf
// isn't simulated (water.slang), and the simulation is fed them round its window's edge (shore_sim.slang), so the two
// agree where one hands over to the other.

// --- Noise -----------------------------------------------------------------------------------------------------------

public float hash(float2 at)
{
    float3 p = frac(float3(at.xyx) * 0.1031);
    p += dot(p, p.yzx + 33.33);
    return frac((p.x + p.y) * p.z);
}

public float value_noise(float2 at)
{
    const float2 cell = floor(at);
    float2 t = at - cell;
    t = t * t * (3.0 - 2.0 * t);
    return lerp(lerp(hash(cell), hash(cell + float2(1.0, 0.0)), t.x), lerp(hash(cell + float2(0.0, 1.0)), hash(cell + float2(1.0, 1.0)), t.x), t.y);
}

static const float kPeriodS = 256.0; // the wind's period, which the waves' time wraps to
static const float kTwoPi = 6.28318531;

// The swell that makes the surf.
public struct Swell
{
    public float2 side;     // along the shore: across the wind, a fixed way (the way to land turns from texel to texel)
    public float period_s;
    public float height_m;  // half its height, out where it starts to feel the bottom; none: no surf
};

// How far into the land the water is drawn: as far as any swash may run up the beach, and a little more. Past it, the
// land hides the water anyway; this keeps it from showing where the ground dips below the sea's level farther in.
public static const float kSwashReachM = 70.0;
// The surf: where it starts to rise out of the open sea's, and where it's all there.
public static const float kSurfFromM = 200.0;
public static const float kSurfFullM = 70.0;

// --- Shore wave bands -----------------------------------------------------------------------------------------------
//
// The waves that run in and wash up (after Alexander Sannikov's breaking waves in Path of Exile 2, ExileCon 2023, in
// our own terms): the water near the shore cut into bands by the distance to land, each carrying one wave, the bands
// moving in toward the land. A wave's front is bent along the shore, but never out of its own band, so fronts never
// cross and a point only ever has two waves over it: the one whose band it's in, if it's behind that one's front, and
// the one ahead of it toward land. Their settings run 0 to 1, so none can be pushed past where this holds.
public static const float kBandWidthM = 45.0;      // how far apart the waves come in
static const float kFrontBend = 1.0;        // 0 to 1: how far a front bends within its band; 1, to its edges
static const float kFrontBendLengthM = 160.0; // along the shore, the size of a front's broad bends
public static const float kBandsReachM = 280.0;     // how far out from the shore there are bands
// Every point is in one wave's sheet, the newer's: the wave whose front is the nearest toward land (fronts stay in order,
// each in its own band). The sheet runs back to the next wave's front, where that one's takes over. The older wave's
// sheet goes on under it, its tail, for 0 to 1 of the most it can (two bands: then a third wave's front would be
// over the point, and there are only two).
public static const float kTailLength = 0.5;

// One wave over a point: which it is (counting out from the shore, the same wave as it comes in), and how far the
// point is behind its front, seaward, in bands: 0 at the front to 2 at the most (the front ahead of it is at most two
// bands away).
public struct ShoreWave
{
    public float index;
    public float behind;
};

public struct ShoreWaves
{
    public ShoreWave newer; // the one whose band the point is in, or, ahead of its front, the next one in
    public ShoreWave older; // the one ahead of it, toward land
};

// Where the bands are: the distance to land in bands, plus how far they've come in. Their period is the swell's,
// rounded to repeat with the wind's, so they never jump.
public float band_phase(float distance_m, float time_s, float period_s)
{
    const float base = kTwoPi / kPeriodS;
    const float omega = max(round(kTwoPi / max(period_s, 1.0) / base), 1.0) * base;
    return distance_m / kBandWidthM + omega * time_s / kTwoPi;
}

// How a front bends along the shore, -1 to 1: smooth noise at two sizes, each wave its own (its row of the noise), so
// a front runs ahead here and lags there, and no two are alike. Stretched, then held to -1 to 1: the bound the bands
// rely on.
float front_bend(float i, float along_m)
{
    const float broad = value_noise(float2(along_m / kFrontBendLengthM, i * 7.31));
    const float fine = value_noise(float2(along_m / (kFrontBendLengthM * 0.31), i * 3.17 + 50.0));
    return clamp(((0.7 * broad + 0.3 * fine) * 2.0 - 1.0) * 1.6, -1.0, 1.0);
}

// Where wave i's front is, in band units: within its band, [i, i + 1], bent along the shore.
public float wave_front(float i, float along_m)
{
    return i + 0.5 + 0.5 * kFrontBend * front_bend(i, along_m);
}

// The last of it: everything a little distorted, so fronts, lips and foam edges never run quite smoothly. Noise at two
// sizes, fixed in the world, moves where a point lies among the bands (a few meters in and out) and along the shore;
// the waves move through it, so their fronts wobble as they come in. Both waves over a point see the same distortion,
// so the fronts still never cross.
static const float kWarpM = 2.5;        // how far, at most, in and out
static const float kWarpSizeM = 11.0;   // the distortion's broad size; its fine one, a third of it

// It eases off toward the waterline, so the water running up and draining back meets the beach in a smooth edge, not
// a jagged one.
public float2 surf_warp(float2 xz, float distance_m)
{
    const float2 broad = float2(value_noise(xz / kWarpSizeM), value_noise(xz / kWarpSizeM + 17.3)) - 0.5;
    const float2 fine = float2(value_noise(xz / (kWarpSizeM * 0.33) + 5.1), value_noise(xz / (kWarpSizeM * 0.33) + 41.7)) - 0.5;
    const float2 warp_m = (broad * 0.7 + fine * 0.3) * 2.0 * kWarpM * smoothstep(4.0, 40.0, distance_m);
    return float2(warp_m.x / kBandWidthM, warp_m.y); // into phase, and along the shore in meters
}

// The two waves over a point distance_m from land.
public ShoreWaves shore_waves(Swell swell, float2 xz, float distance_m, float time_s)
{
    const float2 warp = surf_warp(xz, distance_m);
    const float phase = band_phase(distance_m, time_s, swell.period_s) + warp.x;
    const float along_m = dot(xz, swell.side) + warp.y;
    const float band = floor(phase);
    const float front = wave_front(band, along_m);
    const float newer = phase >= front ? band : band - 1.0;
    ShoreWaves waves;
    waves.newer.index = newer;
    waves.newer.behind = phase - (phase >= front ? front : wave_front(newer, along_m));
    waves.older.index = newer - 1.0;
    waves.older.behind = phase - wave_front(newer - 1.0, along_m);
    return waves;
}

// --- The surf, from the bands ---------------------------------------------------------------------------------------
//
// Each band's wave is the surf: out where it's deep, a round swell; where it's shallow enough to break (a wave breaks
// at a depth about its height over 0.78), a bore: a steep front, the water piled up behind it falling away seaward,
// a white lip on it and foam left behind, thinning. The swell comes in sets: each wave its own height, so a few big ones
// come together, then smaller. Heights are each wave's at its front, read off its two neighbours: the one the point
// is behind and the one coming in toward it from the sea.
static const float kSetWaves = 7.0;      // a set comes every this many waves
static const float kBreakDepthRatio = 0.78;
static const float kSwellWidth = 0.22;   // a round crest's width, in bands
static const float kBoreFace = 0.02;     // a bore's front: how far ahead of it the water starts to rise, in bands
static const float kBoreFall = 2.5;      // how fast the water behind a bore falls away, per band
public static const float kLipWidth = 0.035;    // the white lip, in bands
public static const float kTrailFall = 5.0;     // how fast the foam behind a broken wave thins, per band

// The larger of a and b, rounded over where they're within k of each other (a polynomial smooth maximum).
public float smooth_max(float a, float b, float k)
{
    const float h = max(k - abs(a - b), 0.0) / k;
    return max(a, b) + h * h * k * 0.25;
}

// A wave's share of the swell's height, by its place in its set (0.25 to 1).
public float wave_share(float index)
{
    return 0.62 + 0.38 * sin(kTwoPi * index / kSetWaves + hash(float2(floor(index / kSetWaves), 1.7)) * 2.0);
}

// The water's height at a point of the surf from the waves around it, and how broken they are there (0 to 1).
public float surf_height(float phase, float along_m, float amplitude_m, float broken)
{
    const float band = floor(phase);
    const float front = wave_front(band, along_m);
    const float behind_index = phase >= front ? band : band - 1.0;
    const float behind = phase - wave_front(behind_index, along_m);
    const float ahead = wave_front(behind_index + 1.0, along_m) - phase;
    const float swell_behind = exp(-(behind * behind) / (kSwellWidth * kSwellWidth));
    const float swell_ahead = exp(-(ahead * ahead) / (kSwellWidth * kSwellWidth));
    const float bore_behind = exp(-behind * kBoreFall);
    const float bore_ahead = exp(-ahead / kBoreFace);
    const float rise_behind = lerp(swell_behind, bore_behind, broken) * wave_share(behind_index);
    const float rise_ahead = lerp(swell_ahead, bore_ahead, broken) * wave_share(behind_index + 1.0);
    // Blended where they cross, not the higher of the two: a plain max leaves a crease there, a sharp line the light
    // picks out along every wave.
    return amplitude_m * (smooth_max(rise_behind, rise_ahead, 0.35) - 0.3);
}

// What the surf is like at a distance from land and a depth.
public struct SurfShape
{
    public float strength;    // 0 to 1: how much surf there is here
    public float amplitude_m; // its waves' height
    public float broken;      // 0 to 1: swell to bore
};

// A wave's height: the swell's, growing as it feels the bottom (Green's law); it breaks where the depth is about its
// height over 0.78, and from there the depth holds it to 0.78 of itself, so broken waves shrink toward the shore. Its
// crest stands most of that above the sea's level.
public SurfShape surf_shape(Swell swell, float distance_m, float depth_m)
{
    SurfShape shape;
    shape.strength =
        smoothstep(kSurfFromM, kSurfFullM, distance_m) * (1.0 - smoothstep(kBandsReachM * 0.7, kBandsReachM, distance_m));
    const float shoaled_m = 2.0 * swell.height_m * clamp(pow(max(depth_m, 0.5) / 20.0, -0.25), 1.0, 1.6);
    const float wave_m = min(shoaled_m, kBreakDepthRatio * depth_m + 0.15);
    shape.amplitude_m = wave_m * shape.strength;
    const float break_depth_m = shoaled_m / kBreakDepthRatio;
    shape.broken = smoothstep(break_depth_m * 1.3, break_depth_m * 0.9, depth_m);
    return shape;
}

// The surf's height as the shallows let it be: a crest dies out over the last few meters to the waterline; a trough,
// which would pull the water down below the beach, goes sooner, so the sand never shows through the sea. By the distance
// to land, which the shore map holds to the centimeter, not its depth, which it holds in quarter meters: a fade by depth
// would end in the map's squares.
public float surf_shallows(float height_m, float distance_m)
{
    return height_m >= 0.0 ? height_m * smoothstep(0.0, 8.0, distance_m) : height_m * smoothstep(6.0, 40.0, distance_m);
}

// --- The swash -------------------------------------------------------------------------------------------------------
//
// What each wave does once it reaches the waterline: it runs up the beach as a sheet and drains back. The sheet is the
// water raised to a level over the land, flat; the beach rising through it is where its tip is, so the tip follows the
// real ground. The level rises and falls as a thrown ball does (the swash is near enough ballistic): quickly up, back
// down, over most of the time until the next wave arrives. How high it runs is a share of the wave's height, so the
// big waves of a set run farthest. It's fresh foam as it rushes up, thinning as it drains.
static const float kSwashRunup = 0.3;  // of a wave's height, how high its swash runs above the sea's level
static const float kSwashTime = 0.85;  // of the time between waves, how long a wave's swash lasts

public struct Swash
{
    public float level_m; // above the sea's level
    public float foam;    // 0 to 1
    public float index;   // which wave it is
};

// At a point signed_m from the waterline (out to sea positive, into the land negative).
public Swash swash_at(Swell swell, float2 xz, float signed_m, float time_s)
{
    Swash swash;
    swash.level_m = 0.0;
    swash.foam = 0.0;
    swash.index = 0.0;
    if (swell.height_m <= 0.0 || signed_m > 30.0 || signed_m < -kSwashReachM)
    {
        return swash;
    }
    // The waves as they reach the waterline here: the one that last did, and how long ago, as a share of the time
    // between waves (a band a wave).
    // Both waves' swash, the higher: where, along the shore, the one that last arrived changes from one wave to the
    // next, the older's is still running, so the level never jumps.
    // Their foam blended by how high each runs, so where the higher changes from one to the other the foam fades
    // across rather than switching in a line.
    const ShoreWaves waves = shore_waves(swell, xz, 0.0, time_s);
    const ShoreWave both[2] = {waves.newer, waves.older};
    float foam_sum = 0.0;
    float weight_sum = 0.0;
    for (int i = 0; i < 2; ++i)
    {
        const float t = both[i].behind / kSwashTime;
        if (t <= 0.0 || t >= 1.0)
        {
            continue;
        }
        const float height_m = 2.0 * swell.height_m * wave_share(both[i].index);
        const float level_m = kSwashRunup * height_m * 4.0 * t * (1.0 - t);
        const float weight = level_m * level_m;
        foam_sum += (1.0 - t) * smoothstep(0.0, 0.05, t) * weight;
        weight_sum += weight;
        if (level_m > swash.level_m)
        {
            swash.level_m = level_m;
            swash.index = both[i].index;
        }
    }
    swash.foam = weight_sum > 0.0 ? foam_sum / weight_sum : 0.0;
    return swash;
}

// How much of a wave's swash a point out to sea still rides up on: all of it at the waterline, none 15 m out.
public float swash_share(float signed_m)
{
    return 1.0 - smoothstep(0.0, 15.0, signed_m);
}

// --- Both together ---------------------------------------------------------------------------------------------------

// The surface these waves make, about the sea's level, at a point: the surf's height out to sea, and the swash's level
// over the last of the sea and up the beach (flat: the ground rising through it is its edge). What the water draws
// where the surf isn't simulated, less the open sea's own waves.
public float shore_waves_surface(Swell swell, float2 xz, float distance_m, float signed_m, float depth_m, float time_s)
{
    float height_m = 0.0;
    if (signed_m >= 0.0 && swell.height_m > 0.0)
    {
        const SurfShape shape = surf_shape(swell, distance_m, depth_m);
        if (shape.strength > 0.0)
        {
            const float2 warp = surf_warp(xz, distance_m);
            const float phase = band_phase(distance_m, time_s, swell.period_s);
            height_m = surf_shallows(surf_height(phase + warp.x, dot(xz, swell.side) + warp.y, shape.amplitude_m, shape.broken),
                                     distance_m);
        }
    }
    return max(height_m, swash_at(swell, xz, signed_m, time_s).level_m * swash_share(signed_m));
}
