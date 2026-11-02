#pragma once

#include "asset/format.h"
#include "core/enum_flags.h"

#include <cstdint>

namespace de::asset {

// Texel formats of cooked textures. Numbers are stored in files: never renumber, only add.
enum class TextureFormat : uint16_t {
    Unknown = 0,
    RGBA8Unorm = 1,
    RGBA8Srgb = 2,
    BC1Unorm = 3, // rgb, 4 bits per texel
    BC1Srgb = 4,
    BC4Unorm = 5, // one channel (r), 4 bits per texel
    BC5Unorm = 6, // two channels (r, g), 8 bits per texel: normal maps (x, y)
    BC7Unorm = 7, // rgba, 8 bits per texel
    BC7Srgb = 8,
};

// Texels per block side: 4 for block-compressed formats, 1 otherwise; 0 for Unknown.
uint32_t texture_block_dim(TextureFormat format);
// Bytes per block (per texel for uncompressed formats); 0 for Unknown.
uint32_t texture_block_bytes(TextureFormat format);
const char* texture_format_name(TextureFormat format);

enum class TextureFlags : uint16_t {
    None = 0,
    NormalMap = 1 << 0, // tangent-space x and y in r and g; shaders rebuild z
    Alpha = 1 << 1,     // alpha holds data (coverage, opacity); otherwise it is 1
};
DE_ENUM_FLAGS(TextureFlags)

// One mip level: tightly packed rows of blocks, ready to copy into a staging buffer.
struct TextureMip {
    uint32_t offset = 0; // from the start of the file, 16-byte aligned
    uint32_t size = 0;   // row_bytes * rows
    uint32_t width = 0;  // texels
    uint32_t height = 0;
    uint32_t row_bytes = 0; // one row of blocks
    uint32_t rows = 0;      // rows of blocks
};

inline constexpr uint32_t kMaxTextureMips = 16; // 32768 texels on a side

// .detex (docs/design.md, Asset formats): a 2D texture with its full mip chain, GPU-ready (block
// compressed by the cooker, which also filters the mips).
struct TextureAsset {
    static constexpr AssetType kType = AssetType::Texture;
    static constexpr uint16_t kFormatVersion = 1;

    AssetHeader header;
    TextureFormat format = TextureFormat::Unknown;
    TextureFlags flags = TextureFlags::None;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t reserved = 0;
    OffsetSpan<TextureMip> mips; // largest first

    // Every mip's size follows from the format and the dimensions, and its data lies in the file.
    core::Result<void> validate(std::span<const std::byte> file) const;

    std::span<const std::byte> mip_data(const void* file, uint32_t mip) const {
        const TextureMip& level = mips.get(file)[mip];
        return {static_cast<const std::byte*>(file) + level.offset, level.size};
    }
};
static_assert(sizeof(TextureAsset) == 88);

} // namespace de::asset
