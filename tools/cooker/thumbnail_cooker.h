#pragma once

#include "asset/asset_id.h"
#include "asset/material_format.h"
#include "asset/mesh_format.h"
#include "cooker/texture_cooker.h"

#include <cstdint>
#include <span>

namespace de::core {
class JobSystem;
}

namespace de::cooker {

// How a material slot looks in a thumbnail: its base colour, times its base colour texture's texels when it has one.
struct ThumbnailLook {
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // linear
    const Image* texture = nullptr;                 // sRGB texels (a small copy: see thumbnail_texture)
    bool masked = false;                            // alpha tested (or blended): cut out under the cutoff
    float alpha_cutoff = 0.5f;
};

// A model's picture (asset::kThumbnailSize square, sRGB, alpha clear where the model isn't): its coarsest LOD still
// sharp at that size, seen from the front (+Z), a little to the left and above, framed on what it covers, lit by one
// light from above and an ambient; back faces lit as front ones (leaves, cards). looks: one per material slot of the
// mesh. False if nothing was drawn.
bool draw_thumbnail(const asset::MeshAsset& mesh, std::span<const ThumbnailLook> looks, Image& out);

// A small copy of a base colour texture for draw_thumbnail: box-filtered down to at most 256 texels a side.
Image thumbnail_texture(const Image& image);

// A texture's picture: the image fitted into the square (its shape kept, clear around it), its texels as they are
// (colour, data and normal maps alike).
bool draw_texture_thumbnail(const Image& image, Image& out);

// How a material looks, for its picture.
struct MaterialLook {
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f}; // linear
    float roughness = 1.0f;
    float metallic = 0.0f;
    const Image* base_color_texture = nullptr; // sRGB texels (small: thumbnail_texture)
    const Image* normal_texture = nullptr;     // tangent space, +Y up (glTF's), small
    bool masked = false;
    float alpha_cutoff = 0.5f;
};

// A material's picture: a sphere wearing it (its texture twice round, once up and down), lit by one light from the
// upper left with a highlight as rough or smooth as it is, clear around it.
bool draw_material_thumbnail(const MaterialLook& look, Image& out);

// The picture as its .detex (BC7 sRGB, one mip), with the colour of the model's edges spread into the clear texels
// round them, so filtering doesn't darken its outline.
core::Result<core::Array<std::byte>> cook_thumbnail(Image& picture, asset::AssetId id, core::JobSystem* jobs,
                                                    std::string_view path, Diagnostics& diagnostics);

} // namespace de::cooker
