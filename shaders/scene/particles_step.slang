// One step of the particles (docs/design.md, VFX foundation: fixed steps), in compute: a group of 64 threads to a
// block of the pool, for the blocks of every emitter instance that takes this step (the CPU lists them).
//
// A slot whose turn in its emitter's ring comes up this step gets a new particle, whatever it held: born part-way
// through the step, as far along as its place among those born in it, where the emitter was then on its way from its
// last step's place to this one's, and moved on for what's left of the step. So a fast emitter, or a slow frame, leaves
// an even trail and not a clump per step. Every other live particle ages by the step, goes if its life is over, and
// otherwise is moved by its emitter's step modules, in their order.
//
// Nothing here depends on the frame rate, a thread's turn or another particle: the same steps give the same pool.

import scene.particles_common;
import scene.scene_common;
import scene.wind;

struct ParticleStepPush
{
    SceneFrame* frame;
    Particle* particles;
    ParticleEmitter* emitters;
    ParticleEmitterStep* steps;
    uint* words;
    uint* blocks;     // a block each, with its ParticleEmitterStep above kParticleBlockBits
    uint block_count; // as many groups are dispatched
    float step_s;
    uint2 reserved;
};

[[vk::push_constant]]
ConstantBuffer<ParticleStepPush> push;

float word_float(uint at)
{
    return asfloat(push.words[at]);
}

// Any direction, each as likely as another.
float3 random_direction(float u, float v)
{
    const float y = 1.0 - 2.0 * u;
    const float r = sqrt(saturate(1.0 - y * y));
    const float angle = kParticleTau * v;
    return float3(r * cos(angle), y, r * sin(angle));
}

// A direction within a cone about an axis (unit), each as likely as another: cos_half is the cosine of its half angle.
float3 cone_direction(float3 axis, float cos_half, float u, float v)
{
    const float cos_angle = lerp(cos_half, 1.0, u);
    const float sin_angle = sqrt(saturate(1.0 - cos_angle * cos_angle));
    const float3 aside = abs(axis.y) < 0.99 ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 tangent = normalize(cross(aside, axis));
    const float3 bitangent = cross(axis, tangent);
    const float around = kParticleTau * v;
    return axis * cos_angle + (tangent * cos(around) + bitangent * sin(around)) * sin_angle;
}

// Where a new particle starts and how it moves, in its emitter's axes: the birth run, each module adding to the last.
void birth(ParticleEmitter emitter, uint seed, out float3 position, out float3 velocity)
{
    position = float3(0.0);
    velocity = float3(0.0);
    uint n = kRandomBirth;
    uint at = emitter.birth_first;
    const uint end = at + emitter.birth_words;
    for (uint m = 0; m < kParticleMaxModules && at < end; ++m)
    {
        const uint header = push.words[at];
        const uint type = header & 0xffu;
        if (type == kModuleSphere)
        {
            const float3 direction = random_direction(particle_random(seed, n), particle_random(seed, n + 1));
            // On it, or anywhere inside: the cube root spreads them evenly through its volume.
            const float radius = word_float(at + 2) != 0.0 ? 1.0 : pow(particle_random(seed, n + 2), 1.0 / 3.0);
            position += direction * (radius * word_float(at + 1));
            n += 3;
        }
        else if (type == kModuleDisc)
        {
            const float angle = kParticleTau * particle_random(seed, n);
            const float radius = sqrt(particle_random(seed, n + 1)) * word_float(at + 1);
            position += float3(cos(angle), 0.0, sin(angle)) * radius;
            n += 2;
        }
        else if (type == kModuleBox)
        {
            const float3 at_random =
                float3(particle_random(seed, n), particle_random(seed, n + 1), particle_random(seed, n + 2)) * 2.0 - 1.0;
            position += at_random * float3(word_float(at + 1), word_float(at + 2), word_float(at + 3));
            n += 3;
        }
        else if (type == kModuleVelocity)
        {
            velocity += float3(word_float(at + 1), word_float(at + 2), word_float(at + 3));
        }
        else if (type == kModuleConeVelocity)
        {
            const float3 axis = float3(word_float(at + 1), word_float(at + 2), word_float(at + 3));
            const float3 direction =
                cone_direction(axis, word_float(at + 4), particle_random(seed, n), particle_random(seed, n + 1));
            velocity += direction * lerp(word_float(at + 5), word_float(at + 6), particle_random(seed, n + 2));
            n += 3;
        }
        else if (type == kModuleRandomVelocity)
        {
            const float3 direction = random_direction(particle_random(seed, n), particle_random(seed, n + 1));
            velocity += direction * lerp(word_float(at + 1), word_float(at + 2), particle_random(seed, n + 2));
            n += 3;
        }
        at += header >> 8u;
    }
}

// One octave of the turbulence: the curl of three sine waves, so it swirls without sources or sinks (nothing gathers
// or thins out in it). Each of its parts is within 1.8 of 0.
float3 swirl(float3 q, float t)
{
    static const float3 a = float3(0.3, 1.0, 0.7);
    static const float3 b = float3(0.8, 0.2, 1.0);
    static const float3 c = float3(1.0, 0.6, 0.25);
    const float wave_a = cos(dot(a, q) + t);
    const float wave_b = cos(dot(b, q) + 1.3 * t);
    const float wave_c = cos(dot(c, q) + 0.7 * t);
    return float3(c.y * wave_c - b.z * wave_b, a.z * wave_a - c.x * wave_c, b.x * wave_b - a.y * wave_a);
}

// The turbulence at a point: two octaves, no stronger than 1 anywhere.
float3 turbulence(float3 q, float t)
{
    static const float kMost = 1.5 * 3.0; // an octave's length is under 3; the second adds half as much
    return (swirl(q, t) + 0.5 * swirl(q * 2.17 + 11.3, t * 1.9)) / kMost;
}

// A live particle a time on: older, gone if its life is over, else pushed by the step run and moved. time_s: its
// emitter's clock, which turbulence changes by.
void advance(ParticleEmitter emitter, inout Particle particle, float dt, float time_s)
{
    particle.age += dt;
    if (particle.age >= particle_lifetime(emitter.lifetime, particle.seed))
    {
        particle.age = -1.0;
    }
    else
    {
        const bool local = (emitter.flags & kParticleLocal) != 0u;
        uint at = emitter.step_first;
        const uint end = at + emitter.step_words;
        for (uint m = 0; m < kParticleMaxModules && at < end; ++m)
        {
            const uint header = push.words[at];
            const uint type = header & 0xffu;
            if (type == kModuleAcceleration)
            {
                const float3 world = float3(word_float(at + 1), word_float(at + 2), word_float(at + 3));
                particle.velocity += (local ? world_to_emitter(emitter, world) : world) * dt;
            }
            else if (type == kModuleDrag)
            {
                particle.velocity *= exp(-word_float(at + 1) * dt);
            }
            else if (type == kModuleTurbulence)
            {
                // Each instance in a field of its own, so two of one effect don't sway as one.
                const float3 apart = float3(float(emitter.seed & 0xffu), float((emitter.seed >> 8u) & 0xffu),
                                            float((emitter.seed >> 16u) & 0xffu));
                const float3 q = particle.position * word_float(at + 2) + apart;
                particle.velocity += turbulence(q, time_s * word_float(at + 3)) * (word_float(at + 1) * dt);
            }
            else if (type == kModuleWind)
            {
                const float3 world = particle_world(emitter, push.frame.render_origin_steps.xyz, particle.position);
                const float2 blows = wind_at(push.frame, world.xz, false);
                const float3 wind = float3(blows.x, 0.0, blows.y);
                particle.velocity += ((local ? world_to_emitter(emitter, wind) : wind) - particle.velocity) *
                                     (1.0 - exp(-word_float(at + 1) * dt));
            }
            at += header >> 8u;
        }
        particle.position += particle.velocity * dt;
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void compute_main(uint3 thread: SV_DispatchThreadID)
{
    const uint listed = thread.x / kParticleBlockSize;
    // Nested, with no early return past a read: llvmpipe runs a group's invocations as lanes of one (see scene.cull).
    if (listed < push.block_count)
    {
        const uint entry = push.blocks[listed];
        const ParticleEmitterStep step = push.steps[entry >> kParticleBlockBits];
        const ParticleEmitter emitter = push.emitters[step.emitter];
        const uint index = (entry & kParticleBlockMask) * kParticleBlockSize + thread.x % kParticleBlockSize;
        const uint slot = index - emitter.first_particle;
        if (slot < emitter.capacity)
        {
            Particle particle = push.particles[index];
            // How many slots past the first one born this step this one is, round the ring.
            const uint behind = (slot + emitter.capacity - step.ring_first) % emitter.capacity;
            if (behind < step.born_count)
            {
                particle.seed = particle_seed(emitter.seed, step.serial_first + behind);
                float3 position;
                float3 velocity;
                birth(emitter, particle.seed, position, velocity);
                // Born this far through the step: the first of them early, the last late, evenly between.
                const float through = (float(behind) + 0.5) / float(step.born_count);
                if ((emitter.flags & kParticleLocal) != 0u)
                {
                    particle.position = position;
                    particle.velocity = velocity;
                }
                else
                {
                    const float3 now = float3(emitter.rows[0].w, emitter.rows[1].w, emitter.rows[2].w);
                    const float3 emitter_at = lerp(emitter.from.xyz, now, lerp(step.from_t, step.to_t, through));
                    particle.position = emitter_at + emitter_to_world(emitter, position);
                    particle.velocity = emitter_to_world(emitter, velocity);
                }
                particle.age = 0.0;
                advance(emitter, particle, (1.0 - through) * push.step_s, step.time_s);
            }
            else if (particle.age >= 0.0)
            {
                advance(emitter, particle, push.step_s, step.time_s);
            }
            push.particles[index] = particle;
        }
    }
}
