#pragma once

#include "asset/prefab_format.h"
#include "cooker/source_tree.h"
#include "cooker/world_cooker.h"

#include <span>

namespace de::cooker {

// A .prefab text source (JSON): a composed thing, defined once and placed many times (docs/design.md, Prefabs).
//
//   {
//     "parts": {                                    // each under an ID of its own: a-z, 0-9 and _, at most 31
//       "wax":   {"mesh": "candle.glb", "collision": "mesh"},
//       "flame": {"effect": "../fire/candle_flame.vfx", "position": [0, 0.2, 0]},
//       "table": {"prefab": "../table/table.prefab", "variant": "bare", "position": [1, 0, 0]}
//     },
//     "variants": {                                 // optional: the same prefab with parts left out
//       "unlit": {"flame": null}
//     }
//   }
//
// A part is what a cell's entity can be, placed from the prefab's origin ("position", "rotation" or "rotation_deg",
// "scale", as in a cell):
//     mesh         a model source, relative to the prefab's file; with "collision": "mesh", a static body in its shape
//     effect       an effect source (.vfx), likewise: its emitters play there
//     light        a local light there (cooker/light_source.h has its keys): {"color": [1, 0.6, 0.3], "intensity": 1,
//                  "radius_m": 3}; by itself, or with a mesh or an effect
//     prefab       another prefab, all of it there; "variant" picks one of its variants. Not with a mesh or an effect
// A variant names the parts it leaves out, each as null: a part's ID, or a path into a nested prefab
// ("table/candle/flame"). Leaving out a part that's a prefab leaves out everything in it.
//
// A cell's entity places a prefab with "prefab" (and "variant"): tools/cooker/cell_cooker.h.
//
// What's cooked is flat: the parts of the prefabs it nests are among its own, each named by its path of IDs and
// placed from this prefab's origin, so nothing at run time walks a tree.
struct PrefabBuild {
    core::Array<asset::PrefabPart> parts{tools_heap()};
    core::Array<asset::PrefabVariant> variants{tools_heap()};
    core::Array<char> names{tools_heap()};
    core::Array<asset::LightDesc> lights{tools_heap()}; // the parts' lights, a part's by its index
    core::Array<asset::AssetRef> dependencies{
        tools_heap()}; // its meshes, their collision, its effects: first use first
    // Every source it took something from: the prefabs it nests (their bytes are among its inputs: it cooks again
    // when one changes) and the models and effects its parts name (their IDs are).
    core::Array<SourcePath> read{tools_heap()};
    float reach_m = 0.0f;
};

// Parses and checks a prefab source, reading the prefabs it nests through `read`. Every problem is reported against
// its path, by part; fails if there was any.
core::Result<PrefabBuild> import_prefab(const SourceTree& tree, const Source& source, std::span<const std::byte> bytes,
                                        SourceReader read, Diagnostics& diagnostics);

// The .deprefab.
core::Array<std::byte> cook_prefab(const PrefabBuild& build, asset::AssetId id);

// What a cell that places a prefab depends on besides its ID: a hash of its variants' names, 0 if the source can't be
// read as a prefab. A cell cooks again when a variant comes, goes or is renamed, and not for any other edit.
uint64_t prefab_variants_hash(std::span<const std::byte> bytes);

// Whether a prefab source has a variant of that name.
bool prefab_has_variant(std::span<const std::byte> bytes, std::string_view variant);

} // namespace de::cooker
