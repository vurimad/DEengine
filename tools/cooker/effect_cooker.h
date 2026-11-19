#pragma once

#include "asset/effect_format.h"
#include "cooker/source_tree.h"

#include <span>

namespace de::cooker {

// A .vfx text source (JSON): an effect, as a list of emitters (docs/design.md, VFX foundation).
//
//   {
//     "draw_distance_m": 60,                 // past this from the camera it isn't simulated or drawn (100)
//     "emitters": [
//       {
//         "name": "flame",                   // unique in the effect
//         "space": "world",                  // "world": particles are left behind as the emitter moves (the default);
//                                            // "local": they move with it
//         "rate_per_s": 30,                  // particles born a second
//         "lifetime_s": [0.35, 0.55],        // each lives between these (or one number)
//         "capacity": 24,                    // the most alive at once; worked out from the two above when left out
//         "prewarm_s": 0.5,                  // how long it has run already when first seen (its longest lifetime)
//         "reach_m": 0.6,                    // how far from the emitter a particle can get; worked out when left out
//         "modules": [                       // in order, each {"type": ..., its numbers}
//           {"type": "disc", "radius_m": 0.004},
//           {"type": "cone_velocity", "direction": [0, 1, 0], "angle_deg": 6, "speed_mps": [0.2, 0.3]},
//           {"type": "acceleration", "mps2": [0, 0.5, 0]},
//           {"type": "size", "m": [0.02, 0.03]},
//           {"type": "size_over_life", "keys": [[0, 0.6], [0.3, 1], [1, 0.2]]}
//         ],
//         "look": {"shape": "glow", "opacity": 0, "intensity": 8, "lit": false, "soft_m": 0.05}
//       }
//     ]
//   }
//
// Modules. A number given as "[least, most]" may also be one number, for both.
//   Where a particle starts, in the emitter's axes (each adds to the last; none: at its origin):
//     sphere            radius_m, surface (true: on it; false, the default: anywhere inside)
//     disc              radius_m: flat on the emitter's x and z
//     box               half_extents_m [x, y, z]
//   How it starts moving (each adds to the last; none: at rest):
//     velocity          mps [x, y, z]
//     cone_velocity     direction [x, y, z], angle_deg (the cone's half angle, 0 to 180), speed_mps [least, most]
//     random_velocity   speed_mps [least, most]: in any direction
//   What moves it, every step, in the world's axes:
//     acceleration      mps2 [x, y, z]: gravity ([0, -9.81, 0]), lift
//     drag              per_s: how fast it loses its speed
//     turbulence        strength_mps2, size_m (across its largest swirls), speed_per_s (how fast they change; 0)
//     wind              follow_per_s: how fast it takes up the environment's wind
//   How it looks (it starts 0.05 m across, white, opaque, unturned):
//     size              m [least, most]: across, picked once per particle
//     size_over_life    keys [[t, factor], ...]: t from 0 to 1 of its life, rising; at most 8
//     color             rgb [r, g, b], linear
//     color_over_life   keys [[t, r, g, b], ...]
//     opacity_over_life keys [[t, opacity], ...]: 0 to 1
//     rotation          deg [least, most], spin_dps [least, most]: picked once per particle (both 0 when left out)
//     stretch           seconds (of its motion it's as long as), most (times its width at most; 8)
//
// Look: shape "glow" (bright middle, soft edge), "disc" (even, soft rim), "puff" (eaten away by noise) or "spark" (a
// thin streak); opacity 0 (only adds light) to 1 (covers what's behind); intensity (times its colour); lit (by the sun
// and sky; false: its colour is its own light); soft_m (how far from a surface it starts to fade into it).
struct EffectBuild {
    core::Array<asset::EffectEmitter> emitters{tools_heap()};
    core::Array<uint32_t> words{tools_heap()};
    core::Array<char> names{tools_heap()};
    float draw_distance_m = 100.0f;
};

// Parses and checks an effect source. Every problem is reported against its path, by emitter and module; fails if
// there was any.
core::Result<EffectBuild> import_effect(const Source& source, std::span<const std::byte> bytes,
                                        Diagnostics& diagnostics);

// The .devfx file.
core::Array<std::byte> cook_effect(const EffectBuild& build, asset::AssetId id);

} // namespace de::cooker
