#pragma once

#include "asset/asset_id.h"
#include "cooker/settings.h"

#include <cstdint>
#include <span>

namespace de::core {
class JobSystem;
}

namespace de::cooker {

// Decoded texels: rgba8, rows top to bottom.
struct Image {
    uint32_t width = 0;
    uint32_t height = 0;
    core::Array<uint8_t> rgba{tools_heap()};
};

// PNG, JPEG, TGA or BMP (stb_image), as rgba8 whatever the source's channels; 16-bit sources are
// reduced to 8 bits.
core::Result<Image> decode_image(std::span<const std::byte> encoded, std::string_view path, Diagnostics& diagnostics);

// The .detex of an image: scaled to max_size, mips filtered from the full image (sRGB-correct for
// color, renormalized for normal maps), then block compressed. Blocks encode in parallel on jobs
// (null: on this thread).
core::Result<core::Array<std::byte>> cook_texture(const Image& image, const TextureSettings& settings,
                                                  asset::AssetId id, core::JobSystem* jobs, std::string_view path,
                                                  Diagnostics& diagnostics);

} // namespace de::cooker
