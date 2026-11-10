#include "cooker/thumbnail_cooker.h"

#include "asset/texture_format.h"
#include "core/profile.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

namespace de::cooker {
namespace {

constexpr uint32_t kSize = asset::kThumbnailSize;
constexpr uint32_t kSupersample = 2; // drawn this many times larger each way, then averaged: smooth edges
constexpr uint32_t kDrawn = kSize * kSupersample;
constexpr uint32_t kTextureMaxSide = 256;

struct Float3 {
    constexpr Float3() = default;
    constexpr Float3(float x_in, float y_in, float z_in) : x(x_in), y(y_in), z(z_in) {}
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

Float3 operator+(Float3 a, Float3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
Float3 operator*(Float3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
float dot(Float3 a, Float3 b) {
    return (a.x * b.x) + (a.y * b.y) + (a.z * b.z);
}
Float3 cross(Float3 a, Float3 b) {
    return {(a.y * b.z) - (a.z * b.y), (a.z * b.x) - (a.x * b.z), (a.x * b.y) - (a.y * b.x)};
}
Float3 normalized(Float3 v) {
    const float length = std::sqrt(dot(v, v));
    return length > 1e-12f ? v * (1.0f / length) : Float3(0.0f, 1.0f, 0.0f);
}

// Octahedral: two snorm16, x low.
Float3 decode_normal(uint32_t packed) {
    const auto snorm = [](uint32_t bits) {
        return std::max(static_cast<float>(static_cast<int16_t>(static_cast<uint16_t>(bits))) / 32767.0f, -1.0f);
    };
    Float3 n(snorm(packed & 0xFFFFU), snorm(packed >> 16U), 0.0f);
    n.z = 1.0f - std::abs(n.x) - std::abs(n.y);
    if (n.z < 0.0f) {
        const float x = (1.0f - std::abs(n.y)) * (n.x >= 0.0f ? 1.0f : -1.0f);
        const float y = (1.0f - std::abs(n.x)) * (n.y >= 0.0f ? 1.0f : -1.0f);
        n.x = x;
        n.y = y;
    }
    return normalized(n);
}

float srgb_to_linear(float c) {
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
float linear_to_srgb(float c) {
    c = std::clamp(c, 0.0f, 1.0f);
    return c <= 0.0031308f ? c * 12.92f : (1.055f * std::pow(c, 1.0f / 2.4f)) - 0.055f;
}

} // namespace

Image thumbnail_texture(const Image& image) {
    uint32_t step = 1;
    while (std::max(image.width, image.height) / step > kTextureMaxSide) {
        step *= 2;
    }
    Image small;
    small.width = std::max(1U, image.width / step);
    small.height = std::max(1U, image.height / step);
    small.rgba.resize(static_cast<size_t>(small.width) * small.height * 4);
    for (uint32_t y = 0; y < small.height; ++y) {
        for (uint32_t x = 0; x < small.width; ++x) {
            float sum[4] = {};
            uint32_t count = 0;
            for (uint32_t sy = 0; sy < step && (static_cast<size_t>(y) * step) + sy < image.height; ++sy) {
                for (uint32_t sx = 0; sx < step && (static_cast<size_t>(x) * step) + sx < image.width; ++sx) {
                    const size_t at = ((((static_cast<size_t>(y) * step) + sy) * image.width) +
                                       (static_cast<size_t>(x) * step) + sx) *
                                      4;
                    for (size_t k = 0; k < 3; ++k) {
                        sum[k] += srgb_to_linear(static_cast<float>(image.rgba[at + k]) / 255.0f);
                    }
                    sum[3] += static_cast<float>(image.rgba[at + 3]) / 255.0f;
                    ++count;
                }
            }
            const size_t out = ((static_cast<size_t>(y) * small.width) + x) * 4;
            const float scale = 1.0f / static_cast<float>(std::max(count, 1U));
            for (size_t k = 0; k < 3; ++k) {
                small.rgba[out + k] = static_cast<uint8_t>(std::lround(linear_to_srgb(sum[k] * scale) * 255.0f));
            }
            small.rgba[out + 3] = static_cast<uint8_t>(std::lround(std::clamp(sum[3] * scale, 0.0f, 1.0f) * 255.0f));
        }
    }
    return small;
}

bool draw_thumbnail(const asset::MeshAsset& mesh, std::span<const ThumbnailLook> looks, Image& out) {
    DE_PROFILE_ZONE("draw thumbnail");
    out.width = kSize;
    out.height = kSize;
    out.rgba.resize(static_cast<size_t>(kSize) * kSize * 4);
    std::ranges::fill(out.rgba, uint8_t{0});
    const void* file = &mesh;
    const std::span<const asset::MeshVertex> vertices = mesh.vertices.get(file);
    const std::span<const uint32_t> indices = mesh.indices.get(file);
    const std::span<const asset::MeshLod> lods = mesh.lods.get(file);
    const std::span<const asset::MeshPart> parts = mesh.parts.get(file);
    if (vertices.empty() || lods.empty()) {
        return false;
    }

    // Seen from the front (+Z), a little to the left and above; lit from above, front left.
    const Float3 to_eye = normalized({-0.55f, 0.45f, 1.0f});
    const Float3 forward = to_eye * -1.0f;
    const Float3 right = normalized(cross(forward, {0.0f, 1.0f, 0.0f}));
    const Float3 up = cross(right, forward);
    const Float3 light = normalized({-0.35f, 0.85f, 0.55f});

    // Framed on what it covers.
    float min_x = std::numeric_limits<float>::max();
    float max_x = std::numeric_limits<float>::lowest();
    float min_y = min_x;
    float max_y = max_x;
    for (const asset::MeshVertex& vertex : vertices) {
        const Float3 p(vertex.position[0], vertex.position[1], vertex.position[2]);
        min_x = std::min(min_x, dot(p, right));
        max_x = std::max(max_x, dot(p, right));
        min_y = std::min(min_y, dot(p, up));
        max_y = std::max(max_y, dot(p, up));
    }
    const float extent = std::max(max_x - min_x, max_y - min_y);
    if (!(extent > 0.0f)) {
        return false;
    }
    const float scale = (static_cast<float>(kDrawn) * 0.9f) / extent;
    const float middle_x = (min_x + max_x) * 0.5f;
    const float middle_y = (min_y + max_y) * 0.5f;

    // The coarsest LOD no further from the model than a drawn pixel.
    const float pixel_m = 1.0f / scale;
    uint32_t lod = 0;
    while (lod + 1 < lods.size() && lods[lod + 1].error <= pixel_m) {
        ++lod;
    }

    core::Array<float> screen(tools_heap()); // x, y, depth per vertex
    screen.resize(vertices.size() * 3);
    for (size_t i = 0; i < vertices.size(); ++i) {
        const Float3 p(vertices[i].position[0], vertices[i].position[1], vertices[i].position[2]);
        screen[(i * 3) + 0] = ((dot(p, right) - middle_x) * scale) + (static_cast<float>(kDrawn) * 0.5f);
        screen[(i * 3) + 1] = (static_cast<float>(kDrawn) * 0.5f) - ((dot(p, up) - middle_y) * scale);
        screen[(i * 3) + 2] = dot(p, forward);
    }
    core::Array<float> depth(tools_heap());
    depth.resize(static_cast<size_t>(kDrawn) * kDrawn);
    std::ranges::fill(depth, std::numeric_limits<float>::max());
    core::Array<float> color(tools_heap()); // linear rgb, and 1 where covered
    color.resize(static_cast<size_t>(kDrawn) * kDrawn * 4);
    std::ranges::fill(color, 0.0f);

    const asset::MeshLod& level = lods[lod];
    for (uint32_t p = level.first_part; p < level.first_part + level.part_count && p < parts.size(); ++p) {
        const asset::MeshPart& part = parts[p];
        const ThumbnailLook look = part.material_slot < looks.size() ? looks[part.material_slot] : ThumbnailLook{};
        const Image* texture = look.texture != nullptr && look.texture->width != 0 ? look.texture : nullptr;
        const uint32_t end =
            std::min<uint32_t>(part.first_index + part.index_count, static_cast<uint32_t>(indices.size()));
        for (uint32_t t = part.first_index; t + 3 <= end; t += 3) {
            const uint32_t corner[3] = {indices[t], indices[t + 1], indices[t + 2]};
            if (corner[0] >= vertices.size() || corner[1] >= vertices.size() || corner[2] >= vertices.size()) {
                continue;
            }
            const float* a = &screen[static_cast<size_t>(corner[0]) * 3];
            const float* b = &screen[static_cast<size_t>(corner[1]) * 3];
            const float* c = &screen[static_cast<size_t>(corner[2]) * 3];
            const float area = ((b[0] - a[0]) * (c[1] - a[1])) - ((b[1] - a[1]) * (c[0] - a[0]));
            if (std::abs(area) < 1e-8f) {
                continue;
            }
            const float top = static_cast<float>(kDrawn) - 1.0f;
            const auto low_x = static_cast<int>(std::max(0.0f, std::floor(std::min({a[0], b[0], c[0]}))));
            const auto high_x = static_cast<int>(std::min(top, std::ceil(std::max({a[0], b[0], c[0]}))));
            const auto low_y = static_cast<int>(std::max(0.0f, std::floor(std::min({a[1], b[1], c[1]}))));
            const auto high_y = static_cast<int>(std::min(top, std::ceil(std::max({a[1], b[1], c[1]}))));
            const asset::MeshVertex& va = vertices[corner[0]];
            const asset::MeshVertex& vb = vertices[corner[1]];
            const asset::MeshVertex& vc = vertices[corner[2]];
            const Float3 normal[3] = {decode_normal(va.normal), decode_normal(vb.normal), decode_normal(vc.normal)};
            for (int y = low_y; y <= high_y; ++y) {
                for (int x = low_x; x <= high_x; ++x) {
                    const float px = static_cast<float>(x) + 0.5f;
                    const float py = static_cast<float>(y) + 0.5f;
                    const float w0 = (((b[0] - px) * (c[1] - py)) - ((b[1] - py) * (c[0] - px))) / area;
                    const float w1 = (((c[0] - px) * (a[1] - py)) - ((c[1] - py) * (a[0] - px))) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 < 0.0f || w1 < 0.0f || w2 < 0.0f) {
                        continue;
                    }
                    const float z = (a[2] * w0) + (b[2] * w1) + (c[2] * w2);
                    const size_t at = (static_cast<size_t>(y) * kDrawn) + static_cast<size_t>(x);
                    if (z >= depth[at]) {
                        continue;
                    }
                    // Its colour: the material's, times the texture's and the vertices'.
                    float albedo[4] = {look.base_color[0], look.base_color[1], look.base_color[2], look.base_color[3]};
                    if (texture != nullptr) {
                        const float u = (va.uv[0] * w0) + (vb.uv[0] * w1) + (vc.uv[0] * w2);
                        const float v = (va.uv[1] * w0) + (vb.uv[1] * w1) + (vc.uv[1] * w2);
                        const auto tx =
                            std::min(texture->width - 1,
                                     static_cast<uint32_t>((u - std::floor(u)) * static_cast<float>(texture->width)));
                        const auto ty =
                            std::min(texture->height - 1,
                                     static_cast<uint32_t>((v - std::floor(v)) * static_cast<float>(texture->height)));
                        const uint8_t* texel = &texture->rgba[((static_cast<size_t>(ty) * texture->width) + tx) * 4];
                        for (size_t k = 0; k < 3; ++k) {
                            albedo[k] *= srgb_to_linear(static_cast<float>(texel[k]) / 255.0f);
                        }
                        albedo[3] *= static_cast<float>(texel[3]) / 255.0f;
                    }
                    if (look.masked && albedo[3] < look.alpha_cutoff) {
                        continue;
                    }
                    for (uint32_t k = 0; k < 3; ++k) {
                        const auto channel = [k](uint32_t packed) {
                            return static_cast<float>((packed >> (8U * k)) & 0xFFU) / 255.0f;
                        };
                        albedo[k] *= (channel(va.color) * w0) + (channel(vb.color) * w1) + (channel(vc.color) * w2);
                    }
                    Float3 n = normalized((normal[0] * w0) + (normal[1] * w1) + (normal[2] * w2));
                    if (dot(n, forward) > 0.0f) {
                        n = n * -1.0f; // its back: lit as its front
                    }
                    const float lit = 0.32f + (0.9f * std::max(0.0f, dot(n, light)));
                    depth[at] = z;
                    color[(at * 4) + 0] = albedo[0] * lit;
                    color[(at * 4) + 1] = albedo[1] * lit;
                    color[(at * 4) + 2] = albedo[2] * lit;
                    color[(at * 4) + 3] = 1.0f;
                }
            }
        }
    }

    // Averaged down; clear where nothing was drawn.
    bool any = false;
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            float sum[4] = {};
            for (uint32_t sy = 0; sy < kSupersample; ++sy) {
                for (uint32_t sx = 0; sx < kSupersample; ++sx) {
                    const size_t at = ((static_cast<size_t>((y * kSupersample) + sy) * kDrawn) +
                                       (static_cast<size_t>(x) * kSupersample) + sx) *
                                      4;
                    for (size_t k = 0; k < 4; ++k) {
                        sum[k] += color[at + k];
                    }
                }
            }
            if (sum[3] <= 0.0f) {
                continue;
            }
            any = true;
            uint8_t* texel = &out.rgba[((static_cast<size_t>(y) * kSize) + x) * 4];
            for (size_t k = 0; k < 3; ++k) {
                texel[k] = static_cast<uint8_t>(std::lround(linear_to_srgb(sum[k] / sum[3]) * 255.0f));
            }
            texel[3] =
                static_cast<uint8_t>(std::lround(sum[3] / static_cast<float>(kSupersample * kSupersample) * 255.0f));
        }
    }
    return any;
}

bool draw_texture_thumbnail(const Image& image, Image& out) {
    DE_PROFILE_ZONE("draw texture thumbnail");
    if (image.width == 0 || image.height == 0 ||
        image.rgba.size() < static_cast<size_t>(image.width) * image.height * 4) {
        return false;
    }
    out.width = kSize;
    out.height = kSize;
    out.rgba.resize(static_cast<size_t>(kSize) * kSize * 4);
    std::ranges::fill(out.rgba, uint8_t{0});
    // Fitted, its shape kept: the longer side fills the square.
    const float scale = static_cast<float>(kSize) / static_cast<float>(std::max(image.width, image.height));
    const uint32_t width =
        std::min(kSize, std::max(1U, static_cast<uint32_t>(std::lround(static_cast<float>(image.width) * scale))));
    const uint32_t height =
        std::min(kSize, std::max(1U, static_cast<uint32_t>(std::lround(static_cast<float>(image.height) * scale))));
    const uint32_t left = (kSize - width) / 2;
    const uint32_t top = (kSize - height) / 2;
    for (uint32_t y = 0; y < height; ++y) {
        // The source texels this one covers, averaged.
        const auto y0 = static_cast<uint32_t>(static_cast<uint64_t>(y) * image.height / height);
        const uint32_t y1 =
            std::max(y0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(y + 1) * image.height / height));
        for (uint32_t x = 0; x < width; ++x) {
            const auto x0 = static_cast<uint32_t>(static_cast<uint64_t>(x) * image.width / width);
            const uint32_t x1 =
                std::max(x0 + 1, static_cast<uint32_t>(static_cast<uint64_t>(x + 1) * image.width / width));
            uint32_t sum[4] = {};
            uint32_t count = 0;
            for (uint32_t sy = y0; sy < y1 && sy < image.height; ++sy) {
                for (uint32_t sx = x0; sx < x1 && sx < image.width; ++sx) {
                    const size_t at = ((static_cast<size_t>(sy) * image.width) + sx) * 4;
                    for (size_t k = 0; k < 4; ++k) {
                        sum[k] += image.rgba[at + k];
                    }
                    ++count;
                }
            }
            const size_t at = ((static_cast<size_t>(top + y) * kSize) + left + x) * 4;
            for (size_t k = 0; k < 4; ++k) {
                out.rgba[at + k] = static_cast<uint8_t>(sum[k] / std::max(count, 1U));
            }
        }
    }
    return true;
}

bool draw_material_thumbnail(const MaterialLook& look, Image& out) {
    DE_PROFILE_ZONE("draw material thumbnail");
    out.width = kSize;
    out.height = kSize;
    out.rgba.resize(static_cast<size_t>(kSize) * kSize * 4);
    std::ranges::fill(out.rgba, uint8_t{0});
    constexpr float kPi = std::numbers::pi_v<float>;
    constexpr float kRadius = 0.88f; // of the square's half
    const Float3 light = normalized({-0.45f, 0.6f, 0.65f});
    const Float3 to_eye(0.0f, 0.0f, 1.0f);
    const Float3 half_vector = normalized(light + to_eye);
    const float roughness = std::clamp(look.roughness, 0.04f, 1.0f);
    const float roughness4 = roughness * roughness * roughness * roughness;
    const float shininess = std::clamp((2.0f / roughness4) - 2.0f, 1.0f, 2048.0f);
    const float specular_strength = 0.04f + ((1.0f - roughness) * (1.0f - roughness) * 0.6f);
    const float metallic = std::clamp(look.metallic, 0.0f, 1.0f);
    const auto sample = [](const Image& image, float u, float v, float texel[4]) {
        const auto tx =
            std::min(image.width - 1, static_cast<uint32_t>((u - std::floor(u)) * static_cast<float>(image.width)));
        const auto ty =
            std::min(image.height - 1, static_cast<uint32_t>((v - std::floor(v)) * static_cast<float>(image.height)));
        const uint8_t* at = &image.rgba[((static_cast<size_t>(ty) * image.width) + tx) * 4];
        for (size_t k = 0; k < 4; ++k) {
            texel[k] = static_cast<float>(at[k]) / 255.0f;
        }
    };
    const bool has_color = look.base_color_texture != nullptr && look.base_color_texture->width != 0;
    const bool has_normal = look.normal_texture != nullptr && look.normal_texture->width != 0;
    bool any = false;
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            float sum[4] = {};
            for (uint32_t sy = 0; sy < kSupersample; ++sy) {
                for (uint32_t sx = 0; sx < kSupersample; ++sx) {
                    // Where on the sphere, seen straight on: x right, y up, z toward the eye.
                    const float fx = (static_cast<float>((x * kSupersample) + sx) + 0.5f) / static_cast<float>(kDrawn);
                    const float fy = (static_cast<float>((y * kSupersample) + sy) + 0.5f) / static_cast<float>(kDrawn);
                    const float nx = ((fx * 2.0f) - 1.0f) / kRadius;
                    const float ny = (1.0f - (fy * 2.0f)) / kRadius;
                    const float rest = 1.0f - (nx * nx) - (ny * ny);
                    if (rest <= 0.0f) {
                        continue;
                    }
                    Float3 normal(nx, ny, std::sqrt(rest));
                    // Its texture: twice round, once up and down; the seam behind.
                    const float longitude = std::atan2(normal.x, normal.z);
                    const float u = ((longitude / (2.0f * kPi)) + 0.5f) * 2.0f;
                    const float v = std::acos(std::clamp(normal.y, -1.0f, 1.0f)) / kPi;
                    float albedo[4] = {look.base_color[0], look.base_color[1], look.base_color[2], look.base_color[3]};
                    if (has_color) {
                        float texel[4];
                        sample(*look.base_color_texture, u, v, texel);
                        for (size_t k = 0; k < 3; ++k) {
                            albedo[k] *= srgb_to_linear(texel[k]);
                        }
                        albedo[3] *= texel[3];
                    }
                    if (look.masked && albedo[3] < look.alpha_cutoff) {
                        continue;
                    }
                    if (has_normal) {
                        float texel[4];
                        sample(*look.normal_texture, u, v, texel);
                        // Along u (round the sphere) and v (down it).
                        const Float3 tangent = normalized({std::cos(longitude), 0.0f, -std::sin(longitude)});
                        const Float3 bitangent = normalized(cross(normal, tangent));
                        const float tx = (texel[0] * 2.0f) - 1.0f;
                        const float ty = (texel[1] * 2.0f) - 1.0f;
                        const float tz = std::sqrt(std::max(0.0f, 1.0f - (tx * tx) - (ty * ty)));
                        normal = normalized((tangent * tx) + (bitangent * ty) + (normal * tz));
                    }
                    const float diffuse = std::max(0.0f, dot(normal, light));
                    const float specular =
                        diffuse > 0.0f ? std::pow(std::max(0.0f, dot(normal, half_vector)), shininess) : 0.0f;
                    for (size_t k = 0; k < 3; ++k) {
                        const float lit_diffuse = albedo[k] * (1.0f - metallic) * (0.25f + (0.85f * diffuse));
                        const float tint = (1.0f - metallic) + (metallic * albedo[k]);
                        const float lit_specular = specular * (specular_strength + (metallic * 0.6f)) * tint;
                        sum[k] += lit_diffuse + lit_specular + (metallic * albedo[k] * 0.15f);
                    }
                    sum[3] += 1.0f;
                }
            }
            if (sum[3] <= 0.0f) {
                continue;
            }
            any = true;
            uint8_t* texel = &out.rgba[((static_cast<size_t>(y) * kSize) + x) * 4];
            for (size_t k = 0; k < 3; ++k) {
                texel[k] = static_cast<uint8_t>(std::lround(linear_to_srgb(sum[k] / sum[3]) * 255.0f));
            }
            texel[3] =
                static_cast<uint8_t>(std::lround(sum[3] / static_cast<float>(kSupersample * kSupersample) * 255.0f));
        }
    }
    return any;
}

core::Result<core::Array<std::byte>> cook_thumbnail(Image& picture, asset::AssetId id, core::JobSystem* jobs,
                                                    std::string_view path, Diagnostics& diagnostics) {
    // The edges' colour spread into the clear texels round them, a texel a pass.
    constexpr int kPasses = 4;
    const auto width = static_cast<int>(picture.width);
    const auto height = static_cast<int>(picture.height);
    core::Array<uint8_t> next(tools_heap());
    for (int pass = 0; pass < kPasses; ++pass) {
        next.assign(picture.rgba.span());
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const size_t at = ((static_cast<size_t>(y) * picture.width) + static_cast<size_t>(x)) * 4;
                if (picture.rgba[at + 3] != 0 ||
                    (picture.rgba[at] | picture.rgba[at + 1] | picture.rgba[at + 2]) != 0) {
                    continue; // drawn, or filled by an earlier pass
                }
                uint32_t sum[3] = {};
                uint32_t count = 0;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= width || ny >= height) {
                            continue;
                        }
                        const size_t near = ((static_cast<size_t>(ny) * picture.width) + static_cast<size_t>(nx)) * 4;
                        if ((picture.rgba[near] | picture.rgba[near + 1] | picture.rgba[near + 2]) == 0 &&
                            picture.rgba[near + 3] == 0) {
                            continue;
                        }
                        for (size_t k = 0; k < 3; ++k) {
                            sum[k] += picture.rgba[near + k];
                        }
                        ++count;
                    }
                }
                if (count != 0) {
                    for (size_t k = 0; k < 3; ++k) {
                        next[at + k] = static_cast<uint8_t>(std::max(1U, sum[k] / count));
                    }
                }
            }
        }
        picture.rgba.assign(next.span());
    }
    const TextureSettings settings{.compression = TextureCompression::BC7, .srgb = true, .mips = false};
    return cook_texture(picture, settings, id, jobs, path, diagnostics);
}

} // namespace de::cooker
