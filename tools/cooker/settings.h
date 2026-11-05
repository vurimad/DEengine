#pragma once

#include "cooker/json.h"

#include <cstdint>

namespace de::cooker {

// How a model's material uses a texture: picks the texture's defaults when no folder rule or meta
// says otherwise (a normal map compresses to BC5 and stays linear).
enum class TextureUsage : uint8_t {
    Unknown, // no model or material uses it: treated as Color
    Color,   // base color, emissive: sRGB
    Normal,  // tangent-space normal map
    Data,    // linear data (metallic-roughness, occlusion)
};

const char* texture_usage_name(TextureUsage usage);
bool parse_texture_usage(std::string_view text, TextureUsage& usage);

enum class TextureCompression : uint8_t {
    BC7,  // rgba, best quality, 8 bits per texel
    BC5,  // two channels (normal maps)
    BC4,  // one channel (masks, roughness, height)
    BC1,  // rgb, 4 bits per texel, no alpha
    None, // rgba8, uncompressed
};

// The "texture" section of metas and folder rules.
struct TextureSettings {
    TextureCompression compression = TextureCompression::BC7;
    bool srgb = true;
    bool mips = true;
    bool normal_map = false; // renormalize mips; shaders rebuild z
    uint32_t max_size = 0;   // largest side in texels, 0 for no limit: bigger sources are scaled down
};

// The "mesh" section.
struct MeshSettings {
    float scale = 1.0f;     // applied on top of the importer's conversion to meters
    uint32_t lod_count = 8; // LOD 0 included; the chain stops early when a mesh won't simplify
    float lod_ratio = 0.5f; // triangles kept from one LOD to the next
    float lod_error = 0.5f; // largest error a LOD may add, relative to the mesh's size
    bool optimize = true; // vertex cache, overdraw and fetch order
};

// The defaults a usage implies, as a "texture" section (the bottom layer of a texture's settings).
void texture_usage_defaults(TextureUsage usage, JsonBuilder& settings);

// Reads a resolved section; reports bad values and unknown keys against path.
TextureSettings read_texture_settings(yyjson_val* section, std::string_view path, Diagnostics& diagnostics);
MeshSettings read_mesh_settings(yyjson_val* section, std::string_view path, Diagnostics& diagnostics);

} // namespace de::cooker
