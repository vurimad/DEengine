#pragma once

#include "asset/material_format.h"
#include "cooker/model.h"

#include <span>

namespace de::cooker {

class SourceTree;
struct Source;

// A material on its way to a .demat.
struct MaterialBuild {
    core::StringId shader_template = asset::material::kPbrOpaque;
    asset::AlphaMode alpha_mode = asset::AlphaMode::Opaque;
    asset::MaterialFlags flags = asset::MaterialFlags::None;
    core::Array<asset::MaterialParam> params{tools_heap()};
    core::Array<asset::MaterialTexture> textures{tools_heap()};

    // Set again, the last value wins.
    void set_param(core::StringId name, float x, float y = 0.0f, float z = 0.0f, float w = 0.0f);
    void set_texture(core::StringId slot, asset::AssetId texture);
};

// Sorts the names (lookups are binary searches) and writes the .demat; its textures are its
// dependencies.
core::Array<std::byte> cook_material(MaterialBuild& build, asset::AssetId id);

// A model's material: template from the alpha mode, glTF's factors as parameters. texture_ids[i]
// is the asset of ModelData::textures[i].
MaterialBuild material_from_model(const ModelMaterial& material, std::span<const asset::AssetId> texture_ids);

// The template a material without one uses: pbr_opaque, pbr_masked or pbr_blend.
core::StringId default_template(asset::AlphaMode mode);

// A .material text source (JSON):
//     {
//       "alpha_mode": "opaque",              // opaque, masked or blend; picks the template
//       "template": "pbr_opaque",            // optional: another template
//       "double_sided": false,
//       "params": {"base_color": [1, 0.8, 0.6], "roughness": 0.7},   // missing y, z are 0 and w is 1
//       "textures": {"base_color": "rock_albedo.png", "normal": "rock_normal.png"}
//     }
// Texture paths are relative to the material's folder and must be texture sources. Each texture
// used is added to uses (the "normal" slot as a normal map, base_color and emissive as color, the
// rest as data), which picks its compression defaults.
core::Result<MaterialBuild> import_text_material(const SourceTree& tree, const Source& source,
                                                 std::span<const std::byte> bytes, core::Array<TextureUse>& uses,
                                                 Diagnostics& diagnostics);

} // namespace de::cooker
