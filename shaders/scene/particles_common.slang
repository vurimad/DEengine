// Particles (docs/design.md, VFX foundation): what the step (scene.particles_step) and the draw (scene.particles)
// share. Every effect's particles live in one pool, in blocks of kParticleBlockSize; an emitter instance holds a run of
// blocks and fills it as a ring, its Nth particle in slot N modulo its capacity. A particle keeps only where it is, how
// it moves, its age and a seed: its lifetime, size, colour and every other random choice follow from the seed, the same
// every time (particle_random), which is what lets the viewer pause, step back and start again exactly.
//
// An emitter's modules are three runs of words (asset::EffectModule): what's read as a particle is born, at every
// step, and as it's drawn. A record is a header (its type in the low 8 bits, its length in words above) and its
// numbers, floats as their bits. The cooked file was checked record by record when it loaded (asset::EffectAsset), so
// a walk needs no checks of its own; each is bounded by kParticleMaxModules all the same.

import scene.scene_common;

// Matches render::GpuParticle.
public struct Particle
{
    public float3 position; // relative to its emitter's anchor (world space), or in its emitter's axes (local)
    public float age;       // seconds; below 0: no particle here
    public float3 velocity; // m/s, in the same axes
    public uint seed;
};

// An emitter instance, for a frame. Matches render::GpuParticleEmitter.
public struct ParticleEmitter
{
    public float4 rows[3];     // xyz: its axes (rotation and scale) in the world's; w: where it is now, relative to its anchor
    public float4 from;        // xyz: where it was at its last step, likewise; w: how much of it shows (1, less near its draw distance)
    public uint3 anchor_steps; // what its particles' places are relative to, as an instance's position is kept
    public uint flags;         // kParticleLocal, kParticleLit; its shape in bits 8 to 15
    public uint first_particle; // its run in the pool
    public uint capacity;       // how much of the run it uses: its ring's length
    public uint seed;
    public float opacity;   // 0: only adds its light; 1: covers what's behind
    public float2 lifetime; // least, most (s)
    public float intensity;
    public float soft_m;
    public uint birth_first; // its three runs of modules, in the frame's words
    public uint birth_words;
    public uint step_first;
    public uint step_words;
    public uint draw_first;
    public uint draw_words;
    public float since_step_s; // how long ago its last step was, by its effect's own clock
    public uint reserved;
};

// What one step does for one emitter instance. Matches render::GpuParticleEmitterStep.
public struct ParticleEmitterStep
{
    public uint emitter;      // which ParticleEmitter
    public uint ring_first;   // the slot of the first particle born in this step
    public uint born_count;   // how many are
    public uint serial_first; // that first one's number, counted from the emitter's start
    public float from_t;      // how far along the emitter's move since its last step this one starts, and ends
    public float to_t;
    public float time_s;      // the emitter's own clock at the end of this step: what turbulence changes by
    public uint reserved;
};

public static const uint kParticleBlockSize = 64;
// A list's entry: a block in the low bits, the record it goes with above.
public static const uint kParticleBlockBits = 14;
public static const uint kParticleBlockMask = (1u << kParticleBlockBits) - 1u;

public static const uint kParticleLocal = 1u << 0u; // asset::EffectEmitterFlags
public static const uint kParticleLit = 1u << 1u;
public static const uint kParticleShapeShift = 8;
public static const uint kShapeGlow = 0; // asset::EffectShape
public static const uint kShapeDisc = 1;
public static const uint kShapePuff = 2;
public static const uint kShapeSpark = 3;

public static const uint kParticleMaxModules = 16; // asset::EffectLimits
public static const uint kParticleMaxKeys = 8;
public static const float kParticleDefaultSizeM = 0.05;

// asset::EffectModule.
public static const uint kModuleSphere = 1;
public static const uint kModuleDisc = 2;
public static const uint kModuleBox = 3;
public static const uint kModuleVelocity = 4;
public static const uint kModuleConeVelocity = 5;
public static const uint kModuleRandomVelocity = 6;
public static const uint kModuleAcceleration = 16;
public static const uint kModuleDrag = 17;
public static const uint kModuleTurbulence = 18;
public static const uint kModuleWind = 19;
public static const uint kModuleSize = 32;
public static const uint kModuleSizeOverLife = 33;
public static const uint kModuleColor = 34;
public static const uint kModuleColorOverLife = 35;
public static const uint kModuleOpacityOverLife = 36;
public static const uint kModuleRotation = 37;
public static const uint kModuleStretch = 38;

public static const float kParticleTau = 6.2831853;

// A well-mixed 32 bits from 32 (PCG's output step).
public uint particle_hash(uint x)
{
    x = x * 747796405u + 2891336453u;
    const uint word = ((x >> ((x >> 28u) + 4u)) ^ x) * 277803737u;
    return (word >> 22u) ^ word;
}

// A particle's seed: from its emitter instance's and its number.
public uint particle_seed(uint emitter_seed, uint serial)
{
    return particle_hash(emitter_seed ^ particle_hash(serial));
}

// The particle's nth random number, 0 up to but not 1: the same for the same particle every time it's asked.
public float particle_random(uint seed, uint n)
{
    return float(particle_hash(seed + n * 0x9e3779b9u) >> 8u) * (1.0 / 16777216.0);
}

// Which random number is whose: the lifetime's, then the birth run's from kRandomBirth, the draw run's from
// kRandomDraw, so a module added to one run changes nothing in another.
public static const uint kRandomLifetime = 0;
public static const uint kRandomBirth = 1;
public static const uint kRandomDraw = 64;

public float particle_lifetime(float2 lifetime, uint seed)
{
    return lerp(lifetime.x, lifetime.y, particle_random(seed, kRandomLifetime));
}

// A direction in the emitter's axes, in the world's (scaled as the emitter is).
public float3 emitter_to_world(ParticleEmitter emitter, float3 v)
{
    return float3(dot(emitter.rows[0].xyz, v), dot(emitter.rows[1].xyz, v), dot(emitter.rows[2].xyz, v));
}

// A direction in the world's axes, in the emitter's, turned only: its scale left out.
public float3 world_to_emitter(ParticleEmitter emitter, float3 v)
{
    const float3 x = float3(emitter.rows[0].x, emitter.rows[1].x, emitter.rows[2].x);
    const float3 y = float3(emitter.rows[0].y, emitter.rows[1].y, emitter.rows[2].y);
    const float3 z = float3(emitter.rows[0].z, emitter.rows[1].z, emitter.rows[2].z);
    return float3(dot(x, v) / max(length(x), 1e-6), dot(y, v) / max(length(y), 1e-6), dot(z, v) / max(length(z), 1e-6));
}

// Where the emitter's anchor is, relative to the render origin.
public float3 emitter_anchor(ParticleEmitter emitter, uint3 origin_steps)
{
    return float3(int3(emitter.anchor_steps - origin_steps)) * kMetersPerRenderStep;
}

// A particle's place (as it's kept) relative to the render origin.
public float3 particle_world(ParticleEmitter emitter, uint3 origin_steps, float3 position)
{
    const float3 anchor = emitter_anchor(emitter, origin_steps);
    return anchor + ((emitter.flags & kParticleLocal) != 0u ? emitter_to_world(emitter, position) : position);
}
