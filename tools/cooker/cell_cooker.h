#pragma once

#include "asset/cell_format.h"
#include "cooker/common.h"

#include <span>

namespace de::cooker {

class SourceTree;
struct Source;

// A cell on its way to a .decell.
struct CellBuild {
    int32_t coord_x = 0;
    int32_t coord_z = 0;
    core::Array<asset::CellEntity> entities{tools_heap()}; // parents first
    core::Array<char> names{tools_heap()};
    core::Array<asset::CellFogVolume> fog_volumes{tools_heap()};
    core::Array<asset::AssetRef> meshes{tools_heap()};  // the dependencies, in first-use order
    core::Array<SourcePath> mesh_sources{tools_heap()}; // their sources, in the same order
};

// A .cell text source (JSON): the entities placed in one cell of the world grid.
//     {
//       "coord": [0, 0],                          // the cell: x and z in kCellSize (128 m) steps
//       "entities": [
//         {
//           "name": "statue",                     // optional; unique in the cell
//           "fog_volume": {                       // optional: fog here, sized by its scale (half extents);
//             "shape": "box",                     //   "box" (default) or "ellipsoid"
//             "density_per_m": 0.2,               //   extinction per meter at its heart, on top of the world's
//             "edge_m": 2,                        //   how far in from its surface it takes to reach that
//             "tint": [1, 1, 1],                  //   of the light it scatters
//             "noise_amount": 0.8,                //   0 (default): smooth, to 1: noise can eat all of it, leaving gaps
//             "noise_size_m": 8,                  //   how big its noise's largest shapes are
//             "drift_mps": [0.5, 0, 0.2],         //   how its noise moves through it: wind, in the world's axes
//             "height_fade_m": 2                  //   how far down from its top it thins from nothing
//           },
//           "mesh": "../props/statue/statue.gltf", // optional: a model source, relative to the cell
//           "position": [12, 0, 30.5],            // meters; world space, or the parent's
//           "rotation_deg": [0, 45, 0],           // degrees about x, y, z (roll z, then pitch x,
//                                                 // then yaw y), or "rotation": [x, y, z, w]
//           "scale": 1.5,                         // or [x, y, z]; no zeros
//           "parent": "plinth",                   // optional: another entity's name
//           "static": true                        // never moves (StaticTransform). Default: as its
//                                                 // parent; true for roots. A child of something
//                                                 // that moves can't be static.
//         }
//       ]
//     }
// Entities without a parent should lie in their cell (a warning otherwise). Parents are written
// before their children, otherwise in source order.
core::Result<CellBuild> import_cell(const SourceTree& tree, const Source& source, std::span<const std::byte> bytes,
                                    Diagnostics& diagnostics);

// The .decell; its meshes are its dependencies.
core::Array<std::byte> cook_cell(const CellBuild& build, asset::AssetId id);

} // namespace de::cooker
