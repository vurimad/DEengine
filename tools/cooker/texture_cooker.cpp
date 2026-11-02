#include "cooker/texture_cooker.h"

#include "asset/texture_format.h"
#include "cooker/blob_writer.h"
#include "core/job_system.h"
#include "core/profile.h"

#include <bc7enc_rdo/bc7enc.h>
#include <bc7enc_rdo/rgbcx.h>
#include <stb_image.h>
#include <stb_image_resize2.h>

#include <algorithm>
#include <cmath>
#include <mutex>

namespace de::cooker {

namespace {

using asset::TextureFormat;

constexpr uint32_t kMaxTextureSize = 16384;
constexpr uint32_t kBc1Level = 10; // of rgbcx's 0..18: good quality at a fair speed

void init_encoders() {
    static std::once_flag once;
    std::call_once(once, [] {
        rgbcx::init(rgbcx::bc1_approx_mode::cBC1Ideal);
        bc7enc_compress_block_init();
    });
}

TextureFormat format_of(const TextureSettings& settings) {
    switch (settings.compression) {
    case TextureCompression::BC7:
        return settings.srgb ? TextureFormat::BC7Srgb : TextureFormat::BC7Unorm;
    case TextureCompression::BC5:
        return TextureFormat::BC5Unorm;
    case TextureCompression::BC4:
        return TextureFormat::BC4Unorm;
    case TextureCompression::BC1:
        return settings.srgb ? TextureFormat::BC1Srgb : TextureFormat::BC1Unorm;
    case TextureCompression::None:
        break;
    }
    return settings.srgb ? TextureFormat::RGBA8Srgb : TextureFormat::RGBA8Unorm;
}

// One mip's texels and where its blocks go.
struct EncodeJob {
    const uint8_t* texels = nullptr; // rgba8
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t* out = nullptr;
    uint32_t blocks_x = 0;
    uint32_t block_bytes = 0;
    TextureCompression compression = TextureCompression::BC7;
    const bc7enc_compress_block_params* bc7 = nullptr;
};

void encode_block_rows(size_t begin, size_t end, void* context) {
    DE_PROFILE_ZONE("encode texture blocks");
    const EncodeJob& job = *static_cast<const EncodeJob*>(context);
    uint8_t block[16 * 4];
    for (size_t row = begin; row < end; ++row) {
        for (uint32_t bx = 0; bx < job.blocks_x; ++bx) {
            // The 4x4 texels, repeating the last row and column past the edge.
            for (uint32_t y = 0; y < 4; ++y) {
                const uint32_t sy = std::min(static_cast<uint32_t>(row) * 4 + y, job.height - 1);
                for (uint32_t x = 0; x < 4; ++x) {
                    const uint32_t sx = std::min(bx * 4 + x, job.width - 1);
                    std::memcpy(block + static_cast<size_t>(y * 4 + x) * 4,
                                job.texels + (static_cast<size_t>(sy) * job.width + sx) * 4, 4);
                }
            }
            uint8_t* dest = job.out + (row * job.blocks_x + bx) * job.block_bytes;
            switch (job.compression) {
            case TextureCompression::BC7:
                bc7enc_compress_block(dest, block, job.bc7);
                break;
            case TextureCompression::BC5:
                rgbcx::encode_bc5_hq(dest, block, 0, 1, 4);
                break;
            case TextureCompression::BC4:
                rgbcx::encode_bc4_hq(dest, block, 4);
                break;
            case TextureCompression::BC1:
                rgbcx::encode_bc1(kBc1Level, dest, block, false, false);
                break;
            case TextureCompression::None:
                break;
            }
        }
    }
}

// Unit vectors again after filtering (an average of normals is shorter than one).
void renormalize(core::Array<uint8_t>& rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
        float x = static_cast<float>(rgba[i]) / 127.5f - 1.0f;
        float y = static_cast<float>(rgba[i + 1]) / 127.5f - 1.0f;
        float z = static_cast<float>(rgba[i + 2]) / 127.5f - 1.0f;
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length < 1e-6f) {
            x = 0.0f;
            y = 0.0f;
            z = 1.0f;
        } else {
            x /= length;
            y /= length;
            z /= length;
        }
        const auto encode = [](float value) {
            return static_cast<uint8_t>(std::clamp(std::lround((value * 0.5f + 0.5f) * 255.0f), 0L, 255L));
        };
        rgba[i] = encode(x);
        rgba[i + 1] = encode(y);
        rgba[i + 2] = encode(z);
    }
}

// Resizes rgba8 texels: sRGB-correct and alpha-weighted for color, plain for data.
bool resize(const Image& from, uint32_t width, uint32_t height, const TextureSettings& settings, Image& to) {
    to.width = width;
    to.height = height;
    to.rgba.resize_uninit(static_cast<size_t>(width) * height * 4);
    const auto* in = from.rgba.data();
    auto* out = to.rgba.data();
    const int w = static_cast<int>(from.width);
    const int h = static_cast<int>(from.height);
    const int ow = static_cast<int>(width);
    const int oh = static_cast<int>(height);
    const unsigned char* result =
        settings.srgb
            ? stbir_resize_uint8_srgb(in, w, h, 0, out, ow, oh, 0, STBIR_RGBA)
            : stbir_resize_uint8_linear(in, w, h, 0, out, ow, oh, 0, settings.normal_map ? STBIR_4CHANNEL : STBIR_RGBA);
    if (result == nullptr) {
        return false;
    }
    if (settings.normal_map) {
        renormalize(to.rgba);
    }
    return true;
}

} // namespace

core::Result<Image> decode_image(std::span<const std::byte> encoded, std::string_view path, Diagnostics& diagnostics) {
    DE_PROFILE_ZONE("decode image");
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* texels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(encoded.data()),
                                            static_cast<int>(encoded.size()), &width, &height, &channels, 4);
    if (texels == nullptr) {
        diagnostics.error(path, "not a readable PNG, JPEG, TGA or BMP image: %s", stbi_failure_reason());
        return core::fail(core::ErrorCode::CorruptData, "the image could not be decoded");
    }
    Image image;
    image.width = static_cast<uint32_t>(width);
    image.height = static_cast<uint32_t>(height);
    image.rgba.resize_uninit(static_cast<size_t>(image.width) * image.height * 4);
    std::memcpy(image.rgba.data(), texels, image.rgba.size());
    stbi_image_free(texels);
    return image;
}

core::Result<core::Array<std::byte>> cook_texture(const Image& source, const TextureSettings& settings,
                                                  asset::AssetId id, core::JobSystem* jobs, std::string_view path,
                                                  Diagnostics& diagnostics) {
    DE_PROFILE_ZONE("cook texture");
    init_encoders();
    if (source.width == 0 || source.height == 0 || source.width > kMaxTextureSize || source.height > kMaxTextureSize) {
        diagnostics.error(path, "is %ux%u: textures are 1 to %u texels on a side", source.width, source.height,
                          kMaxTextureSize);
        return core::fail(core::ErrorCode::OutOfRange, "the texture is too large");
    }

    // The top mip: the source, or scaled down to max_size.
    Image scaled;
    const Image* top = &source;
    const uint32_t largest = std::max(source.width, source.height);
    if (settings.max_size != 0 && largest > settings.max_size) {
        const double scale = static_cast<double>(settings.max_size) / largest;
        const auto width = std::max(1U, static_cast<uint32_t>(std::lround(source.width * scale)));
        const auto height = std::max(1U, static_cast<uint32_t>(std::lround(source.height * scale)));
        if (!resize(source, width, height, settings, scaled)) {
            diagnostics.error(path, "could not be scaled to max_size %u", settings.max_size);
            return core::fail(core::ErrorCode::Unknown, "the texture could not be scaled");
        }
        top = &scaled;
    }
    const TextureFormat format = format_of(settings);
    bool has_alpha = false;
    for (size_t i = 3; i < top->rgba.size() && !has_alpha; i += 4) {
        has_alpha = top->rgba[i] != 255;
    }
    if (has_alpha && settings.compression == TextureCompression::BC1) {
        diagnostics.warning(path, "has alpha, which bc1 drops: use bc7 to keep it");
    }

    const uint32_t mip_count =
        settings.mips ? static_cast<uint32_t>(std::bit_width(std::max(top->width, top->height))) : 1;
    const uint32_t block_dim = asset::texture_block_dim(format);
    const uint32_t block_bytes = asset::texture_block_bytes(format);
    bc7enc_compress_block_params bc7;
    bc7enc_compress_block_params_init(&bc7);
    if (!settings.srgb) {
        bc7enc_compress_block_params_init_linear_weights(&bc7);
    }

    asset::TextureMip mips[asset::kMaxTextureMips];
    BlobWriter writer(sizeof(asset::TextureAsset));
    Image level;
    core::Array<uint8_t> blocks(tools_heap());
    for (uint32_t i = 0; i < mip_count; ++i) {
        const uint32_t width = std::max(1U, top->width >> i);
        const uint32_t height = std::max(1U, top->height >> i);
        const Image* texels = top;
        if (i > 0) {
            if (!resize(*top, width, height, settings, level)) {
                diagnostics.error(path, "mip %u could not be filtered", i);
                return core::fail(core::ErrorCode::Unknown, "a mip could not be filtered");
            }
            texels = &level;
        } else if (settings.normal_map) {
            level.width = width;
            level.height = height;
            level.rgba.assign(top->rgba.span());
            renormalize(level.rgba);
            texels = &level;
        }
        const uint32_t blocks_x = (width + block_dim - 1) / block_dim;
        const uint32_t rows = (height + block_dim - 1) / block_dim;
        asset::TextureMip& mip = mips[i];
        mip = {.width = width, .height = height, .row_bytes = blocks_x * block_bytes, .rows = rows};
        mip.size = mip.row_bytes * rows;
        if (settings.compression == TextureCompression::None) {
            mip.offset = writer.append_bytes(std::as_bytes(texels->rgba.span()));
            continue;
        }
        blocks.resize_uninit(mip.size);
        EncodeJob job{.texels = texels->rgba.data(),
                      .width = width,
                      .height = height,
                      .out = blocks.data(),
                      .blocks_x = blocks_x,
                      .block_bytes = block_bytes,
                      .compression = settings.compression,
                      .bc7 = &bc7};
        if (jobs != nullptr) {
            jobs->parallel_for(rows, 4, &encode_block_rows, &job, "encode texture");
        } else {
            encode_block_rows(0, rows, &job);
        }
        mip.offset = writer.append_bytes(std::as_bytes(blocks.span()));
    }

    asset::TextureAsset texture;
    texture.format = format;
    texture.flags = (settings.normal_map ? asset::TextureFlags::NormalMap : asset::TextureFlags::None) |
                    (has_alpha && (settings.compression == TextureCompression::BC7 ||
                                   settings.compression == TextureCompression::None)
                         ? asset::TextureFlags::Alpha
                         : asset::TextureFlags::None);
    texture.width = top->width;
    texture.height = top->height;
    texture.mips = writer.append(std::span<const asset::TextureMip>(mips, mip_count));
    return writer.finish(texture, id, {});
}

} // namespace de::cooker
