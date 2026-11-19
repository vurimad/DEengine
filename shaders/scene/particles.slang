// The particles' draw (docs/design.md, VFX foundation: drawing): after the see-through meshes, onto the lit, fogged
// image and the coverage TAA reads, mixed premultiplied as they are (scene.transparent), tested against the scene's
// depth and writing none. One instanced draw: a quad to a particle, 64 particles to a listed block, the blocks of the
// farthest emitter first.
//
// Everything that's the same across a particle is worked out once, in the vertex shader: where it is now (its last
// step, carried on by its velocity for the time since), its size, colour and turn at its age (the draw run), the sun
// and sky on it if it's lit, the fog in front of it. The fragment shader draws its shape, and fades it where it nears
// a surface and the camera.

import common.bindless;
import scene.brdf;
import scene.forward;
import scene.particles_common;
import scene.scene_common;
import scene.sky;

struct ParticleDrawPush
{
    SceneFrame* frame;
    Particle* particles;
    ParticleEmitter* emitters;
    uint* words;
    uint* blocks;        // a block each, with its ParticleEmitter above kParticleBlockBits
    uint depth;          // the scene's depth, to fade into what's near
    uint atlas;          // the sun's shadow atlas
    float3 camera_right; // the camera's axes, for quads that face it
    uint reserved;
    float3 camera_up;
    float near_m;
    uint integrated;   // the froxel grid, integrated (scene.fog_integrate); unused without one
    uint grid_sampler; // linear clamp
    uint fog;          // 1: what's behind was fogged, so this is too
    uint froxels;      // 1: there is a grid
};

[[vk::push_constant]]
ConstantBuffer<ParticleDrawPush> push;

struct VertexOutput
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD;                     // -1 to 1 across the quad
    float view_depth : VIEW_DEPTH;            // meters along the view
    nointerpolation float3 color : COLOR;     // its light, before its shape
    nointerpolation float3 scatter : SCATTER; // the fog's own light in front of it
    nointerpolation float2 shows : SHOWS;     // x: how much of its light shows, y: how much it covers what's behind
    nointerpolation uint look : LOOK;         // its shape in the low 8 bits, bits of its seed above
    nointerpolation float soft_m : SOFT;
};

struct ParticleOutput
{
    float4 color : SV_Target0;    // premultiplied: rgb, over what's behind times (1 - a)
    float4 coverage : SV_Target1; // r and a alike: how much of the pixel is covered (scene.transparent)
};

// Closer to the camera than this a particle is gone, and it fades in over the next stretch: nothing pops through the
// near plane or fills the screen.
static const float kNearFadeFromM = 0.1;
static const float kNearFadeOverM = 0.3;

float word_float(uint at)
{
    return asfloat(push.words[at]);
}

// A curve of one value at t: the record at `at` holds its key count, then t and a value for each. Straight lines
// between keys; the first key's value before it, the last one's after.
float curve(uint at, float t)
{
    const uint keys = min(push.words[at + 1], kParticleMaxKeys);
    float last_t = word_float(at + 2);
    float value = word_float(at + 3);
    float last_value = value;
    for (uint k = 1; k < keys; ++k)
    {
        const float key_t = word_float(at + 2 + k * 2);
        const float key_value = word_float(at + 3 + k * 2);
        if (t > last_t)
        {
            value = lerp(last_value, key_value, saturate((t - last_t) / (key_t - last_t)));
        }
        last_t = key_t;
        last_value = key_value;
    }
    return value;
}

// The same for a colour: t, r, g, b for each key.
float3 color_curve(uint at, float t)
{
    const uint keys = min(push.words[at + 1], kParticleMaxKeys);
    float last_t = word_float(at + 2);
    float3 value = float3(word_float(at + 3), word_float(at + 4), word_float(at + 5));
    float3 last_value = value;
    for (uint k = 1; k < keys; ++k)
    {
        const float key_t = word_float(at + 2 + k * 4);
        const float3 key_value = float3(word_float(at + 3 + k * 4), word_float(at + 4 + k * 4), word_float(at + 5 + k * 4));
        if (t > last_t)
        {
            value = lerp(last_value, key_value, saturate((t - last_t) / (key_t - last_t)));
        }
        last_t = key_t;
        last_value = key_value;
    }
    return value;
}

// One corner of a live particle's quad.
VertexOutput live_vertex(ParticleEmitter emitter, Particle particle, float age, float lifetime, uint vertex_id)
{
    static const float2 kCorners[6] = { float2(-1.0, -1.0), float2(1.0, -1.0), float2(1.0, 1.0),
                                        float2(-1.0, -1.0), float2(1.0, 1.0),  float2(-1.0, 1.0) };
    SceneFrame* frame = push.frame;
    VertexOutput output;
    output.scatter = float3(0.0);

    // How it looks at this age: the draw run, each module on top of the last.
    const float t = age / lifetime;
    const uint seed = particle.seed;
    float size = kParticleDefaultSizeM;
    float3 color = float3(1.0);
    float opacity = 1.0;
    float angle = 0.0;
    float stretch_s = 0.0;
    float stretch_most = 1.0;
    uint n = kRandomDraw;
    uint at = emitter.draw_first;
    const uint end = at + emitter.draw_words;
    for (uint m = 0; m < kParticleMaxModules && at < end; ++m)
    {
        const uint header = push.words[at];
        const uint type = header & 0xffu;
        if (type == kModuleSize)
        {
            size = lerp(word_float(at + 1), word_float(at + 2), particle_random(seed, n));
            n += 1;
        }
        else if (type == kModuleSizeOverLife)
        {
            size *= curve(at, t);
        }
        else if (type == kModuleColor)
        {
            color *= float3(word_float(at + 1), word_float(at + 2), word_float(at + 3));
        }
        else if (type == kModuleColorOverLife)
        {
            color *= color_curve(at, t);
        }
        else if (type == kModuleOpacityOverLife)
        {
            opacity *= curve(at, t);
        }
        else if (type == kModuleRotation)
        {
            angle = lerp(word_float(at + 1), word_float(at + 2), particle_random(seed, n)) +
                    lerp(word_float(at + 3), word_float(at + 4), particle_random(seed, n + 1)) * age;
            n += 2;
        }
        else if (type == kModuleStretch)
        {
            stretch_s = word_float(at + 1);
            stretch_most = word_float(at + 2);
        }
        at += header >> 8u;
    }

    // Where it is now: its last step, carried on by its velocity.
    const bool local = (emitter.flags & kParticleLocal) != 0u;
    const float3 center = particle_world(emitter, frame.render_origin_steps.xyz, particle.position + particle.velocity * emitter.since_step_s);
    const float3 forward = frame.camera_forward.xyz;

    // A quad facing the camera: turned by its angle, or laid along its motion across the view.
    float3 right = push.camera_right * cos(angle) + push.camera_up * sin(angle);
    float3 up = push.camera_up * cos(angle) - push.camera_right * sin(angle);
    float2 half_size = float2(size * 0.5);
    if (stretch_s > 0.0)
    {
        const float3 velocity = local ? emitter_to_world(emitter, particle.velocity) : particle.velocity;
        const float3 across = velocity - forward * dot(velocity, forward);
        const float speed = length(across);
        if (speed > 1e-4)
        {
            up = across / speed;
            right = cross(up, forward);
            half_size.y = min(size + speed * stretch_s, size * stretch_most) * 0.5;
        }
    }
    const float2 corner = kCorners[vertex_id % 6u];
    const float3 world = center + right * (corner.x * half_size.x) + up * (corner.y * half_size.y);
    output.position = mul_rows(frame.view_proj, float4(world, 1.0));
    output.uv = corner;
    output.view_depth = dot(world - frame.camera_position.xyz, forward);

    // Its light: its own, or the sun's (through its shadows) and the sky's on a round speck, half of which faces
    // the sun on average.
    const float center_depth = dot(center - frame.camera_position.xyz, forward);
    float3 light = float3(1.0);
    if ((emitter.flags & kParticleLit) != 0u)
    {
        const float seen = sun_visibility(frame, push.atlas, center, float3(0.0), center_depth);
        const float open = open_sky(frame, center);
        const float3 sun = frame.sun_color.rgb * lerp(frame.shadow_color.rgb * open, float3(1.0), seen);
        const float3 ambient = 0.5 * open *
                               (hemisphere(frame, float3(0.0, 1.0, 0.0)) + hemisphere(frame, float3(0.0, -1.0, 0.0)));
        light = sun * (0.5 / kPi) + ambient;
    }
    output.color = color * light * emitter.intensity;

    // The fog in front of it, at its middle.
    if (push.fog != 0u)
    {
        const float4 clip = mul_rows(frame.view_proj, float4(center, 1.0));
        float3 transmittance;
        float3 in_scatter;
        fog_in_front(frame, push.integrated, push.grid_sampler, push.froxels != 0u, center, clip.xy / max(clip.w, 1e-6),
                     transmittance, in_scatter);
        output.color *= transmittance;
        output.scatter = in_scatter;
    }
    const float shows = opacity * emitter.from.w;
    output.shows = float2(shows, shows * emitter.opacity);
    output.look = ((emitter.flags >> kParticleShapeShift) & 0xffu) | (seed & 0xffffff00u);
    output.soft_m = emitter.soft_m;
    return output;
}

[shader("vertex")]
VertexOutput vertex_main(uint vertex_id: SV_VulkanVertexID, uint instance_id: SV_VulkanInstanceID)
{
    const uint entry = push.blocks[instance_id / kParticleBlockSize];
    const ParticleEmitter emitter = push.emitters[entry >> kParticleBlockBits];
    const uint index = (entry & kParticleBlockMask) * kParticleBlockSize + instance_id % kParticleBlockSize;
    const Particle particle = push.particles[index];
    const float lifetime = particle_lifetime(emitter.lifetime, particle.seed);
    const float age = particle.age + emitter.since_step_s;

    // No particle here, or its life ran out since the last step: six vertices at one point draw nothing.
    VertexOutput output;
    output.position = float4(0.0, 0.0, 0.0, 1.0);
    output.uv = float2(0.0);
    output.view_depth = 0.0;
    output.color = float3(0.0);
    output.scatter = float3(0.0);
    output.shows = float2(0.0);
    output.look = 0u;
    output.soft_m = 0.0;
    // Not an early return: a software GPU runs neighbours as lanes of one (see scene.cull).
    if (index - emitter.first_particle < emitter.capacity && particle.age >= 0.0 && age < lifetime)
    {
        output = live_vertex(emitter, particle, age, lifetime, vertex_id);
    }
    return output;
}

// Smooth noise, 0 to 1, for the puff: two octaves over the quad, each particle its own.
float puff_hash(float2 cell, uint salt)
{
    const uint2 c = uint2(int2(cell) + 4096);
    return float(particle_hash(c.x * 0x8da6b343u ^ c.y * 0xd8163841u ^ salt) & 0xffffu) / 65535.0;
}

float puff_noise(float2 at, uint salt)
{
    const float2 cell = floor(at);
    const float2 f = at - cell;
    const float2 s = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(puff_hash(cell, salt), puff_hash(cell + float2(1.0, 0.0), salt), s.x),
                lerp(puff_hash(cell + float2(0.0, 1.0), salt), puff_hash(cell + float2(1.0, 1.0), salt), s.x), s.y);
}

// How much of the particle there is at a place on its quad: 1 at its fullest, 0 past its edge.
float shape_at(uint look, float2 uv)
{
    const uint shape = look & 0xffu;
    const float r = length(uv);
    if (shape == kShapeDisc)
    {
        return smoothstep(1.0, 0.75, r);
    }
    if (shape == kShapePuff)
    {
        const uint salt = look >> 8u;
        const float eaten = 0.65 * puff_noise(uv * 2.0 + 7.0, salt) + 0.35 * puff_noise(uv * 4.3 + 3.0, salt + 1u);
        return saturate(smoothstep(1.0, 0.2, r) * 1.6 - eaten * 0.9);
    }
    if (shape == kShapeSpark)
    {
        const float across = saturate(1.0 - abs(uv.x));
        return across * across * saturate(1.0 - uv.y * uv.y);
    }
    // A glow: brightest at its middle, down to nothing at its edge with no rim.
    const float inside = saturate(1.0 - r * r);
    return inside * inside;
}

[shader("fragment")]
ParticleOutput fragment_main(VertexOutput input)
{
    // How far behind it the scene is (reverse Z: depth is near / distance; 0: nothing there).
    const float scene_depth = load_texture(push.depth, int2(input.position.xy)).r;
    const float behind_m = scene_depth > 0.0 ? push.near_m / scene_depth - input.view_depth : 1e9;
    const float into_surface = input.soft_m > 0.0 ? saturate(behind_m / input.soft_m) : 1.0;
    const float into_camera = saturate((input.view_depth - kNearFadeFromM) / kNearFadeOverM);
    const float there = shape_at(input.look, input.uv) * into_surface * into_camera;

    const float covered = there * input.shows.y;
    // Its light counts in full, whatever it covers; the fog's own light in front of it by how much it covers.
    const float3 color = input.color * (there * input.shows.x) + input.scatter * covered;
    ParticleOutput output;
    output.color = float4(min(color, kHalfMax), covered);
    // A glow covers nothing and still changes the pixel: it counts by how bright it is.
    const float seen = saturate(max(covered, max(color.r, max(color.g, color.b))));
    output.coverage = float4(seen, 0.0, 0.0, seen);
    return output;
}
