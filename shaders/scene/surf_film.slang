// The surf's film (tools/worldgen/surf_film.py; docs/design.md, Water): a strip of beach running out to sea, simulated
// offline, baked into a texture and played back along every shore. Across the texture is the place across the shore,
// fine at the waterline and coarser toward either end; down it, the time, round a loop of a few waves. The sea's shore
// material carries it as its base color texture, beside the shore map.
//
// It's the same film at every distance from the camera and on every stretch of shore: a place's is by its distance
// to the waterline, which the shore map holds to the centimeter. Nothing is stepped as the game runs, and nothing is
// made of cells.
//
// Played as it is, every wave would be one line the length of the coast, the same all along. So it's varied along the
// shore, by smooth noise over the place along the shore and the time (surf_film_at): a stretch runs ahead or behind,
// so fronts bend; it's larger or smaller, so waves break farther out or closer in and run higher or lower up the
// beach; and out past the shallows only some stretches are breaking, for a wave or so each, the rest coming in as a
// low hump with no foam, as a swell's crests do.
//
// All of it by the place ALONG the shore and the time, none of it by the distance to the shore or by which wave is
// there. Across the shore, then, the film is read as it was baked, only moved or scaled as a whole: a variation that
// changed across the shore (each wave its own size, say, changing between one wave and the next) would stretch the
// film where it changed slowly and squeeze it where it changed fast, and a foam trail's soft end, squeezed, is a
// hard outline round a patch of foam.
//
// The film is for waves kFilmWaveM high: lengths scale by the sea's waves' height over that, and times by that's
// square root, under which the water's equations don't change.
//   r: the surface's height about the sea's level, from kFilmHeightLowM to kFilmHeightHighM
//   g: the water's depth where it's thin, as the square root of its share of kFilmThinM: 0 where it's dry
//   b: foam, 0 to 1
//   a: the surface's slope across the shore (rising seaward positive), from -kFilmSlopeMost to kFilmSlopeMost

import common.bindless;
import scene.scene_common;
import scene.shore_waves;

// What tools/worldgen/surf_film.py bakes.
static const float kFilmWaveM = 1.0;
static const float kFilmLoopS = 51.2;
static const float kFilmSeaM = 220.0;
static const float kFilmShoreU = 0.35;
static const float kFilmStretch = 5.5;
static const float kFilmHeightLowM = -0.5;
static const float kFilmHeightHighM = 2.0;
static const float kFilmThinM = 0.5;
static const float kFilmSlopeMost = 2.0;
static const float kFilmFlowMostMps = 4.0;

static const float kFilmLeastSide = 256.0; // a base color texture this size or more is a film, not a color
static const float kFilmMostScale = 2.0;   // past it the thin water would be deeper than the shore map's ground goes
static const float kFilmPeriodS = 256.0;   // the wind's period, which the frame's time wraps to
static const float kFilmPace = 1.3;        // how much faster than its waves' height makes it the film is played: to the eye,
                                           // surf at its true pace drags
// How the film varies along the shore: over what length, and how long each lasts (the frame's time wraps every
// kFilmPeriodS, so each is a whole share of that).
static const float kFilmBendS = 2.2;           // how far ahead or behind a stretch of shore runs, for good
static const float kFilmOwnBendS = 0.6;        // and on top of that, changing: fronts aren't the same shape twice
static const float kFilmOwnBendLastsS = 16.0;
static const float kFilmBendLengthM = 170.0;
static const float kFilmSizeLow = 0.78;        // a stretch's waves' size, of the film's: from, to
static const float kFilmSizeHigh = 1.15;
static const float kFilmSizeLengthM = 95.0;
static const float kFilmSizeLastsS = 128.0;    // slowly: the surf out there moves in and out as it changes
static const float kFilmBreakLengthM = 80.0;   // the size of the stretches that break, and of those that don't
static const float kFilmBreakLastsS = 12.8;    // about a wave: the next one breaks somewhere else
static const float kFilmFarFromM = 240.0;      // the film dies away between these distances from the waterline: the
static const float kFilmFarToM = 300.0;        // shore map's distance stops at 327 m, and past it would be one flat value
static const float kFilmUnbrokenHeight = 0.8;  // what's left of the film's height where a stretch isn't breaking
// A wave's front face, for the water to shade: by the rounded film's slope, which is broad on a tall wave and next to
// nothing on a bore in the shallows (a narrow step, which would only get a dark line along its front).
static const float kFilmFaceFrom = 0.08;       // the rounded surface's slope where a face starts,
static const float kFilmFaceFull = 0.35;       // and where it's all face
static const float kFilmCrestFromM = 0.55;     // a wave's height (the film's meters) where its crest starts,
static const float kFilmCrestFullM = 1.2;      // and where it's all crest
static const float kFilmBreakFromM = 12.0;     // closer to the waterline than this, every stretch has broken
static const float kFilmBreakToM = 45.0;       // farther than this, only the stretches that are breaking
// Fronts aren't smooth lines: in and out at three sizes (how far, over what length along the front).
static const float kFilmRaggedM[3] = {1.2, 0.5, 0.22};
static const float kFilmRaggedLengthM[3] = {14.0, 4.0, 1.3};
static const float kFilmRaggedLastsS = 6.4;
// Near the sand a wave doesn't arrive all along the beach at once: it comes in at an angle, touching here first and
// running along from there, and one stretch is still going up while the next is draining back. So the film runs
// this much ahead or behind there, over tens of meters along the shore, most of it the same wave after wave (the
// way a beach's bars turn the waves) and some of it changing. Only near the sand, easing off out to sea: out there
// it would bend the waves' lines into zigzags. (That's a change across the shore, which squeezes the film; it's
// kept slow enough that the film is never squeezed by more than a third.)
static const float kFilmStepS = 2.2;
static const float kFilmStepLengthM = 45.0;
static const float kFilmStepLastsS = 51.2;
static const float kFilmStepNearM = 8.0;       // all of it within this far of the waterline,
static const float kFilmStepFarM = 45.0;       // none of it past this
// And the line of foam a wave brings to the sand isn't one even ribbon: thick here, next to none there, each wave's
// its own. Only its foam: the water itself is all there.
static const float kFilmInnerFoamLeast = 0.2;  // of the film's foam, where it's thinnest
static const float kFilmInnerFoamLengthM = 28.0;
static const float kFilmInnerFoamLastsS = 12.8;
// The run-up's tip isn't a line along the beach either: the sheet reaches farther up here and less far there, in lobes
// (how far, over what length along the beach). They change slowly: the next wave's aren't quite the last one's.
static const float kFilmLobeM[3] = {0.9, 0.4, 0.14};
static const float kFilmLobeLengthM[3] = {5.0, 1.6, 0.5};
static const float kFilmLobeLastsS = 25.6;
static const float kFilmLobeReachM = 12.0; // out to sea, where the lobes have gone and the fronts' ragged edge is whole
// How far up the beach the film's largest wave runs (the tool prints it), for the sand it leaves wet.
public static const float kFilmRunUpM = 11.9;

public struct SurfFilm
{
    public float height_m; // the surface about the sea's level
    public float depth_m;  // the water's depth, where it's thin (thin above 0): none where it's dry
    public float thin;     // 0 to 1: how far the depth here is the film's to give (shallow water and the beach)
    public float foam;     // 0 to 1
    public float slope;    // the surface's, across the shore: rising seaward positive
    public float face;     // 0 to 1: how much this is the front face of a tall wave, rearing up before it breaks
    public float crest;    // 0 to 1: how near the top of a tall wave
    public float beach;    // 0 to 1: how far this is the film's beach, at and above its waterline, not its sea
    public float flow_mps; // how fast the water is moving across the shore, seaward positive (read finely only)
};

// Beside it, as the material's emissive texture, the same film rounded off across the shore: a bore's step spread
// into a hump, and in its foam channel how much foam there is around. A front is drawn as the step it is only where
// it's breaking, with foam on and around it; one with none is drawn from this, rounded.
//
// And as its occlusion texture, in r, how fast the film's water is moving across the shore (seaward positive, from
// -kFilmFlowMostMps to kFilmFlowMostMps): the water slides its foam's pattern along with it.
//
// Whether a shore's material carries a film.
public bool has_surf_film(SceneFrame* frame, uint material)
{
    return float(texture_size(frame.materials[material].base_color_texture).x) >= kFilmLeastSide;
}

// The film's scale for waves wave_m high.
public float surf_film_scale(float wave_m)
{
    return clamp(wave_m / kFilmWaveM, 0.25, kFilmMostScale);
}

// Smooth noise over x and a y that wraps every wrap cells.
float wrapped_noise(float2 at, float wrap)
{
    const float2 cell = floor(at);
    float2 t = at - cell;
    t = t * t * (3.0 - 2.0 * t);
    const float y0 = cell.y - wrap * floor(cell.y / wrap);
    const float y1 = (cell.y + 1.0) - wrap * floor((cell.y + 1.0) / wrap);
    return lerp(lerp(hash(float2(cell.x, y0)), hash(float2(cell.x + 1.0, y0)), t.x),
                lerp(hash(float2(cell.x, y1)), hash(float2(cell.x + 1.0, y1)), t.x), t.y);
}

// Smooth noise over a place along the shore and the frame's time, lasting lasts_s (a whole share of kFilmPeriodS).
float shore_noise(float along, float time_s, float lasts_s)
{
    return wrapped_noise(float2(along, time_s / lasts_s), kFilmPeriodS / lasts_s);
}

// Where a place of the film (meters seaward of its waterline) is across a film texture: the inverse of the tool's
// film_place.
float film_u(uint film, float place_m)
{
    const float a = kFilmSeaM / sinh(kFilmStretch * (1.0 - kFilmShoreU));
    const float x = place_m / a;
    const float side = float(texture_size(film).x);
    return clamp(kFilmShoreU + log(x + sqrt(x * x + 1.0)) / kFilmStretch, 0.5 / side, 1.0 - 0.5 / side);
}

SurfFilm film_tap(uint film, float place_m, float v, float scale)
{
    const float4 texel = texture_2d(film).SampleLevel(sampler_state(kSamplerLinearWrap), float2(film_u(film, place_m), v), 0.0);
    SurfFilm out;
    // Nothing past its seaward end, where the open sea's own waves are all there is.
    out.height_m = lerp(kFilmHeightLowM, kFilmHeightHighM, texel.r) * scale * (1.0 - smoothstep(0.9, 1.0, place_m / kFilmSeaM));
    out.depth_m = texel.g * texel.g * kFilmThinM * scale;
    out.thin = 1.0 - smoothstep(0.55, 0.9, texel.g);
    out.foam = texel.b;
    out.slope = (texel.a * 2.0 - 1.0) * kFilmSlopeMost;
    out.face = 0.0;
    out.crest = 0.0;
    out.beach = 0.0;
    out.flow_mps = 0.0;
    return out;
}

// The film at a point signed_m seaward of the waterline (into the land negative) and along_m along the shore (across
// the wind, a fixed way), at the frame's time, for waves wave_m high. footprint_m: how far apart it's being read (the
// water's mesh): past half a meter the film is averaged across that, so a bore's front doesn't flicker between
// vertices.
public SurfFilm surf_film_at(SceneFrame* frame, uint material, float signed_m, float along_m, float time_s, float wave_m,
                             float footprint_m)
{
    const float scale = surf_film_scale(wave_m);
    // The loop, as long as its waves' height makes it, stretched to go a whole number of times into the wind's period.
    const float loops = max(round(kFilmPeriodS * kFilmPace / (kFilmLoopS * sqrt(scale))), 1.0);
    const uint film = frame.materials[material].base_color_texture;

    // How the film is at this stretch of the shore, now.
    const float shared_bend = 0.7 * value_noise(float2(along_m / kFilmBendLengthM, 3.7)) +
                              0.3 * value_noise(float2(along_m / (kFilmBendLengthM * 0.31), 50.0));
    const float own_bend = shore_noise(along_m / (kFilmBendLengthM * 0.6) + 23.1, time_s, kFilmOwnBendLastsS);
    const float step = 0.5 * value_noise(float2(along_m / kFilmStepLengthM, 61.3)) +
                       0.25 * value_noise(float2(along_m / (kFilmStepLengthM * 0.3), 17.9)) +
                       0.25 * shore_noise(along_m / (kFilmStepLengthM * 0.6) + 33.3, time_s, kFilmStepLastsS);
    const float bend_s = kFilmBendS * (shared_bend * 2.0 - 1.0) + kFilmOwnBendS * (own_bend * 2.0 - 1.0) +
                         kFilmStepS * (step * 2.0 - 1.0) * (1.0 - smoothstep(kFilmStepNearM, kFilmStepFarM, signed_m));
    const float size = lerp(kFilmSizeLow, kFilmSizeHigh,
                            smoothstep(0.15, 0.85, shore_noise(along_m / kFilmSizeLengthM + 11.3, time_s, kFilmSizeLastsS)));
    // Eased over nearly all of the noise, with a finer one in it: a breaking stretch thins away toward its ends over
    // tens of meters, unevenly; it isn't a block cut out of the wave.
    const float breaks = 0.75 * shore_noise(along_m / kFilmBreakLengthM + 53.7, time_s, kFilmBreakLastsS) +
                         0.25 * shore_noise(along_m / (kFilmBreakLengthM * 0.21) + 91.3, time_s, kFilmBreakLastsS);
    const float breaking = smoothstep(0.22, 0.8, breaks);
    // A front's ragged edge. The finer sizes only where it's read finely enough to carry them; less of all of it at
    // the waterline.
    float ragged = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        const float carried = saturate(1.0 - footprint_m / (0.8 * kFilmRaggedLengthM[i]));
        ragged += kFilmRaggedM[i] * carried *
                  (shore_noise(along_m / kFilmRaggedLengthM[i] + 7.9 + 23.5 * float(i), time_s, kFilmRaggedLastsS) * 2.0 - 1.0);
    }
    float lobes = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        const float carried = saturate(1.0 - footprint_m / (0.8 * kFilmLobeLengthM[i]));
        lobes += kFilmLobeM[i] * carried *
                 (shore_noise(along_m / kFilmLobeLengthM[i] + 3.3 + 41.7 * float(i), time_s, kFilmLobeLastsS) * 2.0 - 1.0);
    }
    const float wobble_m = ragged * lerp(0.3, 1.0, smoothstep(0.0, 15.0, signed_m)) +
                           lobes * (1.0 - smoothstep(0.0, kFilmLobeReachM, signed_m));

    const float v = (time_s + bend_s) * loops / kFilmPeriodS;
    const float sized = scale * size;
    const float place_m = (signed_m + wobble_m) / sized;
    SurfFilm out;
    if (footprint_m <= 0.5)
    {
        out = film_tap(film, place_m, v, sized);
    }
    else
    {
        const float reach_m = footprint_m / sized / 3.0;
        const SurfFilm a = film_tap(film, place_m - reach_m, v, sized);
        const SurfFilm b = film_tap(film, place_m, v, sized);
        const SurfFilm c = film_tap(film, place_m + reach_m, v, sized);
        out.height_m = (a.height_m + b.height_m + c.height_m) / 3.0;
        out.depth_m = (a.depth_m + b.depth_m + c.depth_m) / 3.0;
        out.thin = (a.thin + b.thin + c.thin) / 3.0;
        out.foam = (a.foam + b.foam + c.foam) / 3.0;
        out.slope = (a.slope + b.slope + c.slope) / 3.0;
        out.face = 0.0;
        out.crest = 0.0;
    }
    out.beach = 1.0 - smoothstep(0.0, 3.0, place_m);
    // The flow, where the film is read finely (per pixel, for the foam): the film's speed, in the game's meters and
    // seconds.
    out.flow_mps = 0.0;
    const uint flowing = frame.materials[material].occlusion_texture;
    if (footprint_m <= 0.5 && float(texture_size(flowing).x) >= kFilmLeastSide)
    {
        const float speed = texture_2d(flowing).SampleLevel(sampler_state(kSamplerLinearWrap), float2(film_u(flowing, place_m), v), 0.0).r;
        out.flow_mps = (speed * 2.0 - 1.0) * kFilmFlowMostMps * sized * loops * kFilmLoopS / kFilmPeriodS;
    }
    // Out past the shallows, only the stretches that are breaking: the rest a low hump, no foam.
    const float broken = lerp(1.0, breaking, smoothstep(kFilmBreakFromM, kFilmBreakToM, signed_m));
    // Near the sand, how much of its foam this stretch of the wave has.
    const float inner = smoothstep(0.25, 0.7, 0.7 * shore_noise(along_m / kFilmInnerFoamLengthM + 5.7, time_s, kFilmInnerFoamLastsS) +
                                                  0.3 * shore_noise(along_m / (kFilmInnerFoamLengthM * 0.27) + 77.1, time_s,
                                                                    kFilmInnerFoamLastsS));
    const float foam_share =
        lerp(lerp(kFilmInnerFoamLeast, 1.0, inner), 1.0, smoothstep(kFilmBreakFromM, kFilmBreakToM, signed_m));
    // A front is the step the film has only where it's breaking, there's foam around it, and the water is deep
    // enough to stand as a wall (a bore out in the surf). Anywhere else (a stretch that isn't breaking, a surge up the
    // beach, the backwash meeting the sea, the last shallows) it's the rounded film's hump: a step in thin or clear
    // water is a hard line with a change of color across it, which water doesn't have. Its foam still marks it.
    const uint rounded = frame.materials[material].emissive_texture;
    if (float(texture_size(rounded).x) >= kFilmLeastSide)
    {
        const SurfFilm hump = film_tap(rounded, place_m, v, sized);
        // By the foam it's drawn with: a stretch with little of it is as round as one with none.
        const float sharp = broken * smoothstep(0.05, 0.3, hump.foam * foam_share) * (1.0 - hump.thin);
        out.height_m = lerp(hump.height_m, out.height_m, sharp);
        out.depth_m = lerp(hump.depth_m, out.depth_m, sharp);
        out.thin = lerp(hump.thin, out.thin, sharp);
        out.slope = lerp(hump.slope, out.slope, sharp);
        out.face = smoothstep(kFilmFaceFrom, kFilmFaceFull, hump.slope);
    }
    else
    {
        out.face = smoothstep(4.0 * kFilmFaceFrom, 4.0 * kFilmFaceFull, out.slope);
    }
    out.crest = smoothstep(kFilmCrestFromM, kFilmCrestFullM, out.height_m / sized);
    // Only where there's water standing: up the beach the film's surface is its dry bed, as high as any crest.
    out.face *= 1.0 - out.thin;
    out.crest *= 1.0 - out.thin;
    const float near = 1.0 - smoothstep(kFilmFarFromM, kFilmFarToM, signed_m);
    const float standing = lerp(kFilmUnbrokenHeight, 1.0, broken) * near;
    out.height_m *= standing;
    out.slope *= standing;
    out.face *= standing;
    out.crest *= standing;
    out.foam *= broken * near * foam_share;
    return out;
}
