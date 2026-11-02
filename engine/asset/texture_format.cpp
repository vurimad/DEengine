#include "asset/texture_format.h"

#include <algorithm>

namespace de::asset {

uint32_t texture_block_dim(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8Unorm:
    case TextureFormat::RGBA8Srgb:
        return 1;
    case TextureFormat::BC1Unorm:
    case TextureFormat::BC1Srgb:
    case TextureFormat::BC4Unorm:
    case TextureFormat::BC5Unorm:
    case TextureFormat::BC7Unorm:
    case TextureFormat::BC7Srgb:
        return 4;
    case TextureFormat::Unknown:
        break;
    }
    return 0;
}

uint32_t texture_block_bytes(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8Unorm:
    case TextureFormat::RGBA8Srgb:
        return 4;
    case TextureFormat::BC1Unorm:
    case TextureFormat::BC1Srgb:
    case TextureFormat::BC4Unorm:
        return 8;
    case TextureFormat::BC5Unorm:
    case TextureFormat::BC7Unorm:
    case TextureFormat::BC7Srgb:
        return 16;
    case TextureFormat::Unknown:
        break;
    }
    return 0;
}

const char* texture_format_name(TextureFormat format) {
    switch (format) {
    case TextureFormat::RGBA8Unorm:
        return "rgba8";
    case TextureFormat::RGBA8Srgb:
        return "rgba8_srgb";
    case TextureFormat::BC1Unorm:
        return "bc1";
    case TextureFormat::BC1Srgb:
        return "bc1_srgb";
    case TextureFormat::BC4Unorm:
        return "bc4";
    case TextureFormat::BC5Unorm:
        return "bc5";
    case TextureFormat::BC7Unorm:
        return "bc7";
    case TextureFormat::BC7Srgb:
        return "bc7_srgb";
    case TextureFormat::Unknown:
        break;
    }
    return "unknown";
}

core::Result<void> TextureAsset::validate(std::span<const std::byte> file) const {
    using core::ErrorCode;
    const uint32_t block_dim = texture_block_dim(format);
    const uint32_t block_bytes = texture_block_bytes(format);
    if (block_dim == 0) {
        return core::fail(ErrorCode::CorruptData, "texture has an unknown format");
    }
    if (!span_in_file(mips, file.size())) {
        return core::fail(ErrorCode::CorruptData, "texture mip table lies outside the file");
    }
    if (width == 0 || height == 0 || mips.count == 0 || mips.count > kMaxTextureMips ||
        (std::max(width, height) >> (mips.count - 1)) == 0) {
        return core::fail(ErrorCode::CorruptData, "texture has no texels, or more mips than texels");
    }
    for (uint32_t i = 0; i < mips.count; ++i) {
        const TextureMip& mip = mips.get(file.data())[i];
        const uint32_t mip_width = std::max(1U, width >> i);
        const uint32_t mip_height = std::max(1U, height >> i);
        const uint32_t row_bytes = (mip_width + block_dim - 1) / block_dim * block_bytes;
        const uint32_t rows = (mip_height + block_dim - 1) / block_dim;
        if (mip.width != mip_width || mip.height != mip_height || mip.row_bytes != row_bytes || mip.rows != rows ||
            static_cast<uint64_t>(mip.size) != static_cast<uint64_t>(row_bytes) * rows) {
            return core::fail(ErrorCode::CorruptData, "texture mip size doesn't follow from its format and level");
        }
        if (mip.offset < sizeof(TextureAsset) || mip.offset % kAssetAlignment != 0 ||
            static_cast<uint64_t>(mip.offset) + mip.size > file.size()) {
            return core::fail(ErrorCode::CorruptData, "texture mip data lies outside the file");
        }
    }
    return {};
}

} // namespace de::asset
