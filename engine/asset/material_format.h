#pragma once

#include "asset/format.h"
#include "core/enum_flags.h"
#include "core/string_id.h"

#include <cstddef>
#include <cstdint>

namespace de::asset {

enum class AlphaMode : uint8_t {
    Opaque = 0,
    Masked = 1, // alpha test against the alpha_cutoff parameter
    Blend = 2,
};

enum class MaterialFlags : uint8_t {
    None = 0,
    DoubleSided = 1 << 0,
};
DE_ENUM_FLAGS(MaterialFlags)

// A named value: scalars use x, colors x, y, z, w.
struct MaterialParam {
    core::StringId name;
    float value[4] = {};
};
static_assert(sizeof(MaterialParam) == 24);

struct MaterialTexture {
    core::StringId slot;
    AssetId texture;
};

// .demat (docs/design.md, Asset formats): a shader template and the values for it. Parameters and
// textures are sorted by name, so lookups are binary searches. Which names a template reads is the
// template's business; the cooker writes the ones the source sets, and templates have defaults.
struct MaterialAsset {
    static constexpr AssetType kType = AssetType::Material;
    static constexpr uint16_t kFormatVersion = 1;

    AssetHeader header;
    core::StringId shader_template; // "pbr_opaque"
    AlphaMode alpha_mode = AlphaMode::Opaque;
    MaterialFlags flags = MaterialFlags::None;
    uint8_t reserved[6] = {};
    OffsetSpan<MaterialParam> params;
    OffsetSpan<MaterialTexture> textures;

    // Arrays inside the file, sorted by name without repeats; a known alpha mode.
    core::Result<void> validate(std::span<const std::byte> file) const;

    // Null when the material doesn't set it.
    const MaterialParam* find_param(const void* file, core::StringId name) const;
    AssetId find_texture(const void* file, core::StringId slot) const;
};
static_assert(sizeof(MaterialAsset) == 96);

// The names the cooker writes for the standard PBR templates (metal-roughness, as in glTF).
namespace material {

template <size_t N>
consteval core::StringId name(const char (&text)[N]) {
    return {text, N - 1};
}

inline constexpr core::StringId kPbrOpaque = name("pbr_opaque");
inline constexpr core::StringId kPbrMasked = name("pbr_masked");
inline constexpr core::StringId kPbrBlend = name("pbr_blend");

// Parameters.
inline constexpr core::StringId kBaseColor = name("base_color"); // linear rgba
inline constexpr core::StringId kEmissive = name("emissive");    // linear rgb
inline constexpr core::StringId kMetallic = name("metallic");
inline constexpr core::StringId kRoughness = name("roughness");
inline constexpr core::StringId kNormalScale = name("normal_scale");
inline constexpr core::StringId kOcclusionStrength = name("occlusion_strength");
inline constexpr core::StringId kAlphaCutoff = name("alpha_cutoff");

// Texture slots (base_color and emissive are sRGB; the others are linear data).
inline constexpr core::StringId kBaseColorTexture = name("base_color");
inline constexpr core::StringId kNormalTexture = name("normal");
inline constexpr core::StringId kMetallicRoughnessTexture = name("metallic_roughness"); // g, b
inline constexpr core::StringId kOcclusionTexture = name("occlusion");                  // r
inline constexpr core::StringId kEmissiveTexture = name("emissive");

} // namespace material

} // namespace de::asset
