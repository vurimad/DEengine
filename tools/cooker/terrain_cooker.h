#pragma once

#include "asset/asset_id.h"
#include "cooker/common.h"

#include <cstdint>
#include <span>
#include <string_view>

namespace de::core {
class JobSystem;
}

namespace de::cooker {

// A .terrain text source (JSON): a height image and how to read it. The image's size is the map's.
//     {
//       "height": "land.heightpng",     // a 16-bit grayscale PNG, relative to this file
//       "meters_per_pixel": 2,          // between samples: 16 divided by a power of 2
//       "lowest_m": -48,                // what pixel 0 means
//       "highest_m": 1517.9,            // what pixel 65535 means
//       "origin": [1, 1]                // optional: where the first sample lies, X east and Z south, meters
//     }
// The image has an extension of its own so it isn't cooked as a texture; it's read as this source's
// dependency, and needs no meta. The range is required: a PNG doesn't store one, and the cooker doesn't guess.
struct TerrainSource {
    SourcePath height{}; // "content/terrain/land.heightpng"
    double meters_per_pixel = 0.0;
    double lowest_m = 0.0;
    double highest_m = 0.0;
    double origin_m[2] = {};
};

core::Result<TerrainSource> import_terrain(std::string_view path, std::span<const std::byte> bytes,
                                           Diagnostics& diagnostics);

// A height image's samples, row by row from the north-west.
struct HeightImage {
    uint32_t width = 0; // samples along X
    uint32_t depth = 0; // samples along Z
    core::Array<uint16_t> samples{tools_heap()};
};

// Whether encoded is a 16-bit grayscale PNG of at least 2 x 2 samples (reported against path if not),
// from its header alone: what --check asks, without decoding it.
bool check_height_image(std::span<const std::byte> encoded, std::string_view path, Diagnostics& diagnostics);

core::Result<HeightImage> decode_height_image(std::span<const std::byte> encoded, std::string_view path,
                                              Diagnostics& diagnostics);

struct CookedTerrain {
    core::Array<std::byte> terrain{tools_heap()}; // the .deterrain
    core::Array<std::byte> heights{tools_heap()}; // the .deheights
    uint32_t levels = 0;
    uint32_t nodes = 0;
};

// The pyramid (engine/asset/terrain_format.h), from the source's spacing up to the root's. A sample is
// exactly the source sample at that point, so a coarse vertex stands where a fine one does, rounded to a
// whole number of its level's steps; past the map's edge it's the nearest source sample. Fails where a
// block's heights don't fit 16 bits above its base, saying where and by how much. Nodes cook on jobs
// (jobs may be null).
core::Result<CookedTerrain> cook_terrain(const TerrainSource& source, const HeightImage& image, asset::AssetId id,
                                         core::JobSystem* jobs, std::string_view path, Diagnostics& diagnostics);

} // namespace de::cooker
