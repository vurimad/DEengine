#pragma once

#include "asset/material_format.h"
#include "cooker/cook_state.h"

#include <cstdint>

namespace de::cooker {

class SourceTree;
struct Source;

// A model as importers hand it to the mesh and material cookers: whole-precision vertices in the
// engine's space (meters, right-handed, Y up, UV origin top left), node transforms applied.
struct ModelVertex {
    float position[3] = {};
    float normal[3] = {0.0f, 1.0f, 0.0f};
    float tangent[4] = {1.0f, 0.0f, 0.0f, 1.0f}; // w: bitangent sign
    float uv[2] = {};
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
};

// Triangles drawn with one material.
struct ModelPrimitive {
    core::Array<ModelVertex> vertices{tools_heap()};
    core::Array<uint32_t> indices{tools_heap()};
    uint32_t material = UINT32_MAX; // into ModelData::materials; none: the default material
};

inline constexpr uint32_t kNoTexture = UINT32_MAX;

// An image a material samples: a source file of its own, or embedded in the model.
struct ModelTexture {
    SourcePath file{};              // "content/knight/knight_albedo.png"; empty if embedded
    uint32_t embedded = kNoTexture; // into ModelData::images
    TextureUsage usage = TextureUsage::Unknown;
};

struct ModelImage {
    core::Array<std::byte> encoded{tools_heap()}; // PNG or JPEG bytes
};

inline constexpr size_t kMaxMaterialName = 127;

// Metal-roughness PBR, as in glTF; FBX materials are mapped onto it.
struct ModelMaterial {
    core::FixedString<kMaxMaterialName> name;
    float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float emissive[3] = {};
    float metallic = 1.0f;
    float roughness = 1.0f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    float alpha_cutoff = 0.5f;
    asset::AlphaMode alpha_mode = asset::AlphaMode::Opaque;
    bool double_sided = false;
    // Into ModelData::textures.
    uint32_t base_color_texture = kNoTexture;
    uint32_t normal_texture = kNoTexture;
    uint32_t metallic_roughness_texture = kNoTexture;
    uint32_t occlusion_texture = kNoTexture;
    uint32_t emissive_texture = kNoTexture;
};

struct ModelData {
    core::Array<ModelPrimitive> primitives{tools_heap()};
    core::Array<ModelMaterial> materials{tools_heap()};
    core::Array<ModelTexture> textures{tools_heap()};
    core::Array<ModelImage> images{tools_heap()};
    core::Array<SourcePath> dependencies{tools_heap()}; // files read besides the source (.bin buffers)
};

// glTF 2.0 (.gltf with its buffers and images beside it, or .glb). The default scene, flattened:
// every mesh node's primitives in world space. Triangles only; missing normals and tangents are
// generated. Images must be sources under content/ (or embedded).
core::Result<ModelData> import_gltf(const platform::Vfs& vfs, const SourceTree& tree, const Source& source,
                                    Diagnostics& diagnostics);

// FBX (ufbx), converted to meters, Y up, right-handed. The scene flattened like glTF's; materials
// mapped to metal-roughness (base color, normal, emissive textures; metallic and roughness as values).
core::Result<ModelData> import_fbx(const platform::Vfs& vfs, const SourceTree& tree, const Source& source,
                                   Diagnostics& diagnostics);

// Fills in what an importer couldn't read: smooth normals where a primitive had none, then tangents
// from the UVs (per-triangle, accumulated per vertex, orthogonalized).
void generate_normals(ModelPrimitive& primitive);
void generate_tangents(ModelPrimitive& primitive);

// Records a texture reference (one entry per file or embedded image, the usages merged), and
// reports a texture used two ways (a normal map also used as color).
uint32_t add_texture(ModelData& model, const ModelTexture& texture, std::string_view path, Diagnostics& diagnostics);

} // namespace de::cooker
