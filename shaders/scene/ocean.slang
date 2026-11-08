// The FFT ocean (docs/design.md, Water; engine/render/scene/ocean.h): the sea's waves from a wave spectrum, three tiles
// of kSize x kSize each, from a long one for the swell down to a short one for ripples, every wave in exactly one of
// them. Each frame, on compute:
//   evolve     each wave of the spectrum turned on in time, as deep water lets it travel, into the eight things the
//              surface needs: its sideways and up displacement, its slopes, and how sideways moves stretch it
//   fft        those back from waves to the surface, row by row and then column by column, in place
//   assemble   the surface: waves (displacement x, y, z and how far the surface folds, the Jacobian) and slopes (x, z,
//              their square for the roughness mips add, and foam where it folds, fading over seconds)
//   mips       both halved down to a texel, for far off
// The spectrum itself is made only when the sea's state changes: JONSWAP's spectrum of the wind's waves over their
// fetch, and a narrower one of the swell's, each spread about its way.

import common.bindless;

// Matches OceanPush in engine/render/scene/ocean.cpp.
struct OceanPush
{
    uint spectrum;        // storage: h0 per cascade (r, g: h0(k); b, a: the conjugate of h0(-k))
    uint fields;          // storage: two layers per cascade, two complex fields each
    uint waves;           // storage, this frame's: the mip written
    uint slopes;
    uint previous_slopes; // sampled: last frame's, for its foam
    uint source_waves;    // mips: the mip above, storage
    uint source_slopes;
    uint direction;       // fft: 0 along rows, 1 along columns
    float time_s;         // wrapped to kPeriodS
    float choppiness;
    float frame_s;
    float wind_speed_mps;
    float2 wind_direction; // toward where it blows (x, z)
    float wind_alpha;      // JONSWAP's alpha and peak for the wind's waves (ocean.cpp works them out)
    float wind_peak;       // rad/s
    float2 swell_direction;
    float swell_alpha;
    float swell_peak;
    float wind_spread;  // the spreading's normalization for the wind's waves, and the swell's
    float swell_spread;
    uint mip_size; // mips: the size of the mip written
    uint has_previous;
};

[[vk::push_constant]]
ConstantBuffer<OceanPush> push;

static const uint kSize = 256;
static const uint kCascades = 3;
static const float kGravity = 9.81;
static const float kTwoPi = 6.28318531;
// The tiles' sizes (kOceanTileM in ocean.h), and where each hands its waves to the next: six of the next tile's waves.
static const float kTileM[3] = {640.0, 101.0, 17.3};
static const float kBandStart[3] = {0.0, 6.0 * kTwoPi / 101.0, 6.0 * kTwoPi / 17.3};
static const float kBandEnd[3] = {6.0 * kTwoPi / 101.0, 6.0 * kTwoPi / 17.3, 1e9};
// The sea repeats over this long (the wind's period): every wave's frequency is rounded to make whole turns in it.
static const float kPeriodS = 256.0;
// The wind's waves and the swell's: how sharply each peaks, and how widely each spreads about its way (cos^2s).
static const float kWindGamma = 3.3;
static const float kSwellGamma = 7.0;
static const float kWindSpreadPower = 8.0;  // ocean.cpp normalizes for these two
static const float kSwellSpreadPower = 40.0;
// Foam where the surface folds: it starts below this Jacobian, is thickest by this much less, and lasts about this long.
static const float kFoamFold = 0.55;
static const float kFoamRange = 0.35;
static const float kFoamLifeS = 3.5;

// --- Complex numbers -------------------------------------------------------------------------------------------------

float2 cmul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

float2 cconj(float2 a)
{
    return float2(a.x, -a.y);
}

// i times a.
float2 cmul_i(float2 a)
{
    return float2(-a.y, a.x);
}

// --- Spectrum --------------------------------------------------------------------------------------------------------

uint hash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// Two normally distributed numbers for a wave, always the same for it.
float2 gaussian(uint3 at)
{
    const uint seed = hash(at.x + hash(at.y + hash(at.z + 0x9e3779b9u)));
    const float u1 = max(float(hash(seed) & 0xffffffu) / 16777216.0, 1e-7);
    const float u2 = float(hash(seed ^ 0x68bc21ebu) & 0xffffffu) / 16777216.0;
    const float r = sqrt(-2.0 * log(u1));
    return float2(r * cos(kTwoPi * u2), r * sin(kTwoPi * u2));
}

// JONSWAP: energy per frequency, m^2 s, for alpha and a peak in rad/s.
float jonswap(float omega, float alpha, float peak, float gamma)
{
    if (omega <= 0.0 || peak <= 0.0 || alpha <= 0.0)
    {
        return 0.0;
    }
    const float sigma = omega <= peak ? 0.07 : 0.09;
    const float r = exp(-(omega - peak) * (omega - peak) / (2.0 * sigma * sigma * peak * peak));
    const float ratio = peak / omega;
    return alpha * kGravity * kGravity / pow(omega, 5.0) * exp(-1.25 * ratio * ratio * ratio * ratio) * pow(gamma, r);
}

// Spreading about a way, cos^2s of half the angle off it, times its normalization.
float spread(float2 k_direction, float2 way, float power, float normalization)
{
    const float c = clamp(dot(k_direction, way), -1.0, 1.0);
    const float half_cos = sqrt(saturate(0.5 * (1.0 + c)));
    return normalization * pow(half_cos, 2.0 * power);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void spectrum_main(uint3 id: SV_DispatchThreadID)
{
    const uint cascade = id.z;
    const float tile = kTileM[cascade];
    const float dk = kTwoPi / tile;
    const float2 k = (float2(id.xy) - float(kSize / 2)) * dk;
    const float k_length = length(k);
    float4 h0 = float4(0.0);
    // The first row and column (-N/2) have no -k on the grid to pair with: left out, as the Nyquist waves they are.
    if (id.x != 0 && id.y != 0 && k_length >= max(kBandStart[cascade], 1e-4) && k_length < kBandEnd[cascade])
    {
        // Energy per unit of k, both of its components: per frequency (deep water: omega^2 = g k), spread over ways,
        // over k's circle.
        const float omega = sqrt(kGravity * k_length);
        const float domega_dk = kGravity / (2.0 * omega);
        const float2 k_direction = k / k_length;
        const float per_omega = jonswap(omega, push.wind_alpha, push.wind_peak, kWindGamma) *
                                    spread(k_direction, push.wind_direction, kWindSpreadPower, push.wind_spread) +
                                jonswap(omega, push.swell_alpha, push.swell_peak, kSwellGamma) *
                                    spread(k_direction, push.swell_direction, kSwellSpreadPower, push.swell_spread);
        const float energy = per_omega * domega_dk / k_length;
        // h0's mean square is half the energy in its square of k: with the conjugate of -k's, h(k) holds all of it.
        const float amplitude = sqrt(energy * dk * dk * 0.5) * 0.70710678;
        const float2 here = gaussian(uint3(id.xy, cascade)) * amplitude;
        // -k's, from its own numbers.
        const uint2 mirror = (kSize - id.xy) % kSize;
        float2 there = float2(0.0);
        {
            const float2 km = -k;
            const float2 km_direction = km / k_length;
            const float per_omega_m = jonswap(omega, push.wind_alpha, push.wind_peak, kWindGamma) *
                                          spread(km_direction, push.wind_direction, kWindSpreadPower, push.wind_spread) +
                                      jonswap(omega, push.swell_alpha, push.swell_peak, kSwellGamma) *
                                          spread(km_direction, push.swell_direction, kSwellSpreadPower, push.swell_spread);
            const float amplitude_m = sqrt(per_omega_m * domega_dk / k_length * dk * dk * 0.5) * 0.70710678;
            there = gaussian(uint3(mirror, cascade)) * amplitude_m;
        }
        h0 = float4(here, cconj(there));
    }
    storage_2d_array(push.spectrum)[uint3(id.xy, cascade)] = h0;
}

// --- Evolve ----------------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void evolve_main(uint3 id: SV_DispatchThreadID)
{
    const uint cascade = id.z;
    const float dk = kTwoPi / kTileM[cascade];
    const float2 k = (float2(id.xy) - float(kSize / 2)) * dk;
    const float k_length = max(length(k), 1e-6);
    const float4 h0 = storage_2d_array(push.spectrum)[uint3(id.xy, cascade)];
    // Deep water's frequency, rounded to whole turns in the sea's period, so it repeats without a seam.
    const float base = kTwoPi / kPeriodS;
    const float omega = round(sqrt(kGravity * k_length) / base) * base;
    const float phase = omega * push.time_s;
    const float2 turn = float2(cos(phase), sin(phase));
    const float2 h = cmul(h0.xy, turn) + cmul(h0.zw, cconj(turn));
    // Up, sideways (crests sharpen toward each other), slopes, and how the sideways moves stretch the surface.
    const float2 dy = h;
    const float2 dx = cmul_i(h) * (-k.x / k_length);
    const float2 dz = cmul_i(h) * (-k.y / k_length);
    const float2 dy_dx = cmul_i(h) * k.x;
    const float2 dy_dz = cmul_i(h) * k.y;
    const float2 dx_dx = h * (k.x * k.x / k_length);
    const float2 dz_dz = h * (k.y * k.y / k_length);
    const float2 dx_dz = h * (k.x * k.y / k_length);
    // Two real fields to a complex one: a + i b, each comes back as one part.
    RWTexture2DArray<float4> fields = storage_2d_array(push.fields);
    fields[uint3(id.xy, cascade * 2)] = float4(dx + cmul_i(dy), dz + cmul_i(dy_dx));
    fields[uint3(id.xy, cascade * 2 + 1)] = float4(dy_dz + cmul_i(dx_dx), dz_dz + cmul_i(dx_dz));
}

// --- FFT -------------------------------------------------------------------------------------------------------------

groupshared float4 fft_line[kSize];

// Back from waves to the surface along one row or column of one layer, in place: radix 2, 128 threads, a butterfly
// each per stage. A texel holds two complex numbers, transformed together.
[shader("compute")]
[numthreads(128, 1, 1)]
void fft_main(uint3 group: SV_GroupID, uint3 thread: SV_GroupThreadID)
{
    RWTexture2DArray<float4> fields = storage_2d_array(push.fields);
    const uint t = thread.x;
    const uint row = group.y;
    const uint layer = group.z;
    for (uint e = t; e < kSize; e += kSize / 2)
    {
        const uint2 at = push.direction == 0 ? uint2(e, row) : uint2(row, e);
        fft_line[reversebits(e) >> 24] = fields[uint3(at, layer)];
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint size = 2; size <= kSize; size <<= 1)
    {
        const uint half_size = size >> 1;
        const uint k = t % half_size;
        const uint a = (t / half_size) * size + k;
        const uint b = a + half_size;
        const float angle = kTwoPi * float(k) / float(size); // positive: back from waves
        const float2 w = float2(cos(angle), sin(angle));
        const float4 top = fft_line[a];
        const float4 bottom = fft_line[b];
        const float4 turned = float4(cmul(w, bottom.xy), cmul(w, bottom.zw));
        GroupMemoryBarrierWithGroupSync();
        fft_line[a] = top + turned;
        fft_line[b] = top - turned;
        GroupMemoryBarrierWithGroupSync();
    }
    for (uint e = t; e < kSize; e += kSize / 2)
    {
        const uint2 at = push.direction == 0 ? uint2(e, row) : uint2(row, e);
        fields[uint3(at, layer)] = fft_line[e];
    }
}

// --- Assemble --------------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void assemble_main(uint3 id: SV_DispatchThreadID)
{
    const uint cascade = id.z;
    RWTexture2DArray<float4> fields = storage_2d_array(push.fields);
    // The spectrum's middle was k = 0: back on the surface, every other texel's sign is turned.
    const float sign = ((id.x + id.y) & 1u) != 0u ? -1.0 : 1.0;
    const float4 first = fields[uint3(id.xy, cascade * 2)] * sign;
    const float4 second = fields[uint3(id.xy, cascade * 2 + 1)] * sign;
    const float lambda = push.choppiness;
    const float3 displacement = float3(first.x * lambda, first.y, first.z * lambda);
    const float jxx = 1.0 + lambda * second.y;
    const float jzz = 1.0 + lambda * second.z;
    const float jxz = lambda * second.w;
    const float jacobian = jxx * jzz - jxz * jxz;
    // The slopes of the surface as it's moved sideways: steeper where it's squeezed.
    const float2 slope = float2(first.w / max(jxx, 0.2), second.x / max(jzz, 0.2));
    float foam = saturate((kFoamFold - jacobian) / kFoamRange);
    if (push.has_previous != 0u)
    {
        const float before = texture_2d_array(push.previous_slopes).Load(int4(int2(id.xy), int(cascade), 0)).w;
        foam = max(foam, before * exp(-push.frame_s / kFoamLifeS));
    }
    storage_2d_array(push.waves)[uint3(id.xy, cascade)] = float4(displacement, jacobian);
    storage_2d_array(push.slopes)[uint3(id.xy, cascade)] = float4(slope, dot(slope, slope), foam);
}

// --- Mips ------------------------------------------------------------------------------------------------------------

[shader("compute")]
[numthreads(8, 8, 1)]
void mips_main(uint3 id: SV_DispatchThreadID)
{
    if (id.x >= push.mip_size || id.y >= push.mip_size)
    {
        return;
    }
    const uint2 at = id.xy * 2;
    RWTexture2DArray<float4> waves = storage_2d_array(push.source_waves);
    RWTexture2DArray<float4> slopes = storage_2d_array(push.source_slopes);
    float4 wave = float4(0.0);
    float4 slope = float4(0.0);
    for (uint i = 0; i < 4; ++i)
    {
        const uint3 texel = uint3(at + uint2(i & 1u, i >> 1u), id.z);
        wave += waves[texel];
        slope += slopes[texel];
    }
    storage_2d_array(push.waves)[uint3(id.xy, id.z)] = wave * 0.25;
    storage_2d_array(push.slopes)[uint3(id.xy, id.z)] = slope * 0.25;
}
