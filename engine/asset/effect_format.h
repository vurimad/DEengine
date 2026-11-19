#pragma once

#include "asset/format.h"
#include "core/enum_flags.h"

#include <bit>
#include <cstdint>
#include <string_view>

namespace de::asset {

// The range of each effect setting: what the cooker accepts, loading checks, and the particle system and its shaders
// rely on (docs/design.md, VFX foundation).
struct EffectLimits {
    static constexpr uint32_t kMaxEmitters = 16;    // in an effect
    static constexpr uint32_t kMaxModules = 16;     // in one of an emitter's three runs
    static constexpr uint32_t kMaxKeys = 8;         // in a curve over a particle's life
    static constexpr uint32_t kMaxCapacity = 65536; // particles of one emitter instance alive at once
    static constexpr float kMaxRatePerS = 100000.0f;
    static constexpr float kMinLifetimeS = 1.0f / 60.0f; // a step of the simulation
    static constexpr float kMaxLifetimeS = 600.0f;
    static constexpr float kMaxPrewarmS = 60.0f;
    static constexpr float kMaxExtentM = 10000.0f; // a spawn shape's radius or half extent
    static constexpr float kMaxSpeedMps = 1000.0f; // a starting speed
    static constexpr float kMaxAccelerationMps2 = 10000.0f;
    static constexpr float kMaxPerS = 1000.0f;           // drag, how fast wind is taken up, turbulence's speed
    static constexpr float kMinTurbulenceSizeM = 0.001f; // across its largest swirls
    static constexpr float kMaxSizeM = 1000.0f;          // a particle, across
    static constexpr float kMaxFactor = 1000.0f;         // a curve's value, a colour's channel, the intensity
    static constexpr float kMaxAngleRad = 1000.0f;       // a starting angle, and a spin in rad/s
    static constexpr float kMaxStretchS = 10.0f;
    static constexpr float kMaxReachM = 100000.0f;
    static constexpr float kMinDrawDistanceM = 1.0f;
    static constexpr float kMaxDrawDistanceM = 100000.0f;
};

// Which of an emitter's three runs a module belongs to: when it's read.
enum class EffectRunKind : uint8_t {
    Birth, // once, as a particle is born: where it starts and how it moves
    Step,  // every step of the simulation: what moves it
    Draw,  // every frame it's drawn: how it looks at its age
};

// A module: one record of a run. A header word (effect_module_header), then its numbers, floats as their bits. Within
// a run they apply in the file's order.
enum class EffectModule : uint8_t {
    // Birth. A particle starts at the emitter's origin, at rest; places are in the emitter's own axes.
    Sphere = 1,         // radius_m, surface (0: anywhere inside, 1: on it)
    Disc = 2,           // radius_m: flat on the emitter's x and z
    Box = 3,            // half extents x, y, z (m)
    Velocity = 4,       // x, y, z (m/s), added
    ConeVelocity = 5,   // direction x, y, z (unit), the cosine of the cone's half angle, speed least, most (m/s)
    RandomVelocity = 6, // speed least, most (m/s): in any direction
    // Step. Accelerations are in the world's axes.
    Acceleration = 16, // x, y, z (m/s2): gravity, lift
    Drag = 17,         // per second: how fast it loses its speed
    Turbulence = 18,   // strength (m/s2), 1 / the size of its largest swirls (per m), how fast they change (per s)
    Wind = 19,         // per second: how fast it takes up the environment's wind
    // Draw. A particle starts kDefaultSizeM across, white, opaque, unturned.
    Size = 32,            // least, most (m, across): picked once per particle
    SizeOverLife = 33,    // a key count, then for each: t (0 to 1 of its life), a factor
    Color = 34,           // r, g, b (linear), multiplied in
    ColorOverLife = 35,   // a key count, then for each: t, r, g, b
    OpacityOverLife = 36, // a key count, then for each: t, opacity (0 to 1)
    Rotation = 37,        // angle least, most (rad), spin least, most (rad/s): picked once per particle
    Stretch = 38,         // how many seconds of its motion it's as long as, and at most how many times its width
};

inline constexpr float kDefaultEffectSizeM = 0.05f;

// The simulation's step (docs/design.md, VFX foundation: fixed steps): what a capacity and a prewarm are counted in.
inline constexpr double kEffectStepsPerS = 60.0;
inline constexpr double kEffectStepS = 1.0 / kEffectStepsPerS;

// A record's first word: its type, and how many words it is with this one.
constexpr uint32_t effect_module_header(EffectModule type, uint32_t words) {
    return static_cast<uint32_t>(type) | (words << 8U);
}
constexpr EffectModule effect_module_type(uint32_t header) {
    return static_cast<EffectModule>(header & 0xffU);
}
constexpr uint32_t effect_module_words(uint32_t header) {
    return header >> 8U;
}
constexpr uint32_t effect_float_word(float value) {
    return std::bit_cast<uint32_t>(value);
}
constexpr float effect_word_float(uint32_t word) {
    return std::bit_cast<float>(word);
}

// Which run a module type belongs to; false for a type there isn't.
bool effect_module_run(EffectModule type, EffectRunKind& run);
// How many words a record of that type is, header included; for a curve, with that many keys. 0 for a type there
// isn't.
uint32_t effect_module_size(EffectModule type, uint32_t keys);
// "turbulence"; "" for a type there isn't.
const char* effect_module_name(EffectModule type);

// How a particle is drawn: shapes the shader draws itself, no texture.
enum class EffectShape : uint8_t {
    Glow,  // bright at its middle, fading to nothing at its edge: flames, embers, motes of light
    Disc,  // an even round spot with a soft rim: dust, droplets
    Puff,  // a soft spot eaten away by noise, each particle differently: smoke, mist
    Spark, // a thin streak, bright along its middle: sparks (with Stretch)
    Count,
};
const char* effect_shape_name(EffectShape shape);

enum class EffectEmitterFlags : uint16_t {
    None = 0,
    Local = 1 << 0, // particles move with the emitter; without it they're left behind as it moves
    Lit = 1 << 1,   // lit by the sun (through its shadows) and the sky; without it, its colour is its own light
};
DE_ENUM_FLAGS(EffectEmitterFlags)

// A run of an emitter's modules, in EffectAsset::words.
struct EffectRun {
    uint32_t first = 0;
    uint32_t words = 0;
};

// One emitter of an effect: how many particles, how long they live, their modules and their look.
struct EffectEmitter {
    uint32_t name_offset = 0; // into EffectAsset::names
    uint16_t name_length = 0; // unique in the effect
    EffectEmitterFlags flags = EffectEmitterFlags::None;
    float rate_per_s = 0.0f;            // particles born a second
    float lifetime_s[2] = {1.0f, 1.0f}; // each lives between these, picked once
    uint32_t capacity = 0;              // the most alive at once: how long its ring in the pool is
    float prewarm_s = 0.0f;             // how long it has been running already when it's first seen
    // No particle's edge gets farther from where the emitter is than reach_m, in still air; wind carries them
    // wind_reach_s further for each m/s it blows. What an instance is culled by.
    float reach_m = 0.0f;
    float wind_reach_s = 0.0f;
    EffectShape shape = EffectShape::Glow;
    uint8_t reserved[3] = {};
    float opacity = 0.0f;   // 0: it only adds its light; 1: it covers what's behind it
    float intensity = 1.0f; // times its colour
    float soft_m = 0.0f;    // how far from a surface it starts fading into it; 0: a hard cut
    EffectRun birth;
    EffectRun step;
    EffectRun draw;
};
static_assert(sizeof(EffectEmitter) == 76);

// .devfx (docs/design.md, VFX foundation): the binary form of a .vfx text source. An effect is a list of emitters;
// each emitter's modules are three runs of words the particle system's shaders walk as they are, so validate() checks
// every record of every run: its type belongs to its run, its length is its type's, its numbers are finite and in
// range. Nothing past this needs to check them again.
struct EffectAsset {
    static constexpr AssetType kType = AssetType::Effect;
    static constexpr uint16_t kFormatVersion = 1;

    AssetHeader header;
    OffsetSpan<EffectEmitter> emitters; // at least one, at most EffectLimits::kMaxEmitters
    OffsetSpan<uint32_t> words;         // every emitter's runs
    OffsetSpan<char> names;             // every emitter's name, back to back, no terminators
    float draw_distance_m = 100.0f;     // past this from the camera it isn't simulated or drawn
    uint32_t reserved = 0;

    core::Result<void> validate(std::span<const std::byte> file) const;

    std::string_view name(const void* file, const EffectEmitter& emitter) const {
        return {names.get(file).data() + emitter.name_offset, emitter.name_length};
    }
};
static_assert(sizeof(EffectAsset) == 64 + 3 * sizeof(OffsetSpan<char>) + 8);

// Checks one run of words: every record's type is of that kind, its length is its type's and it ends inside the run,
// its numbers are finite and within EffectLimits, a curve's keys are in order from 0 to 1, and there are at most
// EffectLimits::kMaxModules of them. What EffectAsset::validate() runs on each run, and the cooker on what it wrote.
core::Result<void> validate_effect_run(std::span<const uint32_t> run, EffectRunKind kind);

} // namespace de::asset
