#include "asset/cell_format.h"
#include "asset/environment_format.h"
#include "asset/material_format.h"
#include "asset/mesh_format.h"
#include "asset/shader_format.h"
#include "asset/texture_format.h"
#include "asset/world_format.h"
#include "cooker/cook_state.h"
#include "cooker/cooker.h"
#include "cooker/mesh_cooker.h"
#include "cooker/shader_cooker.h"
#include "cooker/world_cooker.h"
#include "core/job_system.h"
#include "platform/file.h"
#include "platform/vfs.h"
#include "render/shader/shader_asset.h"
#include "render/shader/shader_compiler.h"
#include "render/shader/shader_library.h"
#include "support/cook_kit.h"
#include "support/gpu.h"
#include "support/sandbox.h"

// NOLINTBEGIN(readability-identifier-naming): stb's configuration macros
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
// NOLINTEND(readability-identifier-naming)

#include <bc7enc_rdo/bc7decomp.h>
#include <bc7enc_rdo/rgbcx.h>
#include <doctest/doctest.h>
#include <stb_image_write.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace de::asset;
using de::cooker::CookReport;
using de::platform::Vfs;
using de::test::bytes_of;
using de::test::Cooked;
using de::test::cooker_vfs;
using de::test::guid_of;
using de::test::has_message;
using de::test::read_text;
using de::test::remove_src;
using de::test::run_cook;
using de::test::Sandbox;
using de::test::write_src;

namespace {

// --- Writing sources --------------------------------------------------------------------------

// An image as PNG: rgba8, rows top to bottom.
std::vector<std::byte> png(uint32_t width, uint32_t height, const std::vector<uint8_t>& rgba) {
    std::vector<std::byte> out;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto& bytes = *static_cast<std::vector<std::byte>*>(context);
            const auto* begin = static_cast<const std::byte*>(data);
            bytes.insert(bytes.end(), begin, begin + size);
        },
        &out, static_cast<int>(width), static_cast<int>(height), 4, rgba.data(), static_cast<int>(width * 4));
    return out;
}

// A smooth color image with some detail, the kind BC7 handles well.
std::vector<uint8_t> color_texels(uint32_t size, uint8_t seed) {
    std::vector<uint8_t> rgba(size_t{size} * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            uint8_t* texel = &rgba[(size_t{y} * size + x) * 4];
            texel[0] = static_cast<uint8_t>(x * 255 / (size - 1));
            texel[1] = static_cast<uint8_t>(y * 255 / (size - 1));
            texel[2] = static_cast<uint8_t>(128 + 60 * std::sin((x + y + seed) * 0.3));
            texel[3] = 255;
        }
    }
    return rgba;
}

// A bumpy tangent-space normal map.
std::vector<uint8_t> normal_texels(uint32_t size) {
    std::vector<uint8_t> rgba(size_t{size} * size * 4);
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            const float nx = 0.4f * std::sin(static_cast<float>(x) * 0.4f);
            const float ny = 0.4f * std::cos(static_cast<float>(y) * 0.3f);
            const float nz = std::sqrt(1.0f - nx * nx - ny * ny);
            uint8_t* texel = &rgba[(size_t{y} * size + x) * 4];
            texel[0] = static_cast<uint8_t>(std::lround((nx * 0.5f + 0.5f) * 255.0f));
            texel[1] = static_cast<uint8_t>(std::lround((ny * 0.5f + 0.5f) * 255.0f));
            texel[2] = static_cast<uint8_t>(std::lround((nz * 0.5f + 0.5f) * 255.0f));
            texel[3] = 255;
        }
    }
    return rgba;
}

std::string base64(const void* data, size_t size) {
    static constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const auto* bytes = static_cast<const uint8_t*>(data);
    std::string out;
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t chunk = (uint32_t{bytes[i]} << 16U) | (i + 1 < size ? uint32_t{bytes[i + 1]} << 8U : 0U) |
                               (i + 2 < size ? uint32_t{bytes[i + 2]} : 0U);
        out += kDigits[(chunk >> 18U) & 63U];
        out += kDigits[(chunk >> 12U) & 63U];
        out += i + 1 < size ? kDigits[(chunk >> 6U) & 63U] : '=';
        out += i + 2 < size ? kDigits[chunk & 63U] : '=';
    }
    return out;
}

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
};

// A glTF with one mesh in one node (translated by node_y), its buffer embedded as a data: URI.
// materials_json, images_json and textures_json are spliced in as they are. With second_indices,
// the mesh has a second primitive over the same vertices, with material 1.
std::string gltf(const std::vector<Vertex>& vertices, const std::vector<uint32_t>& indices, float node_y,
                 const std::string& materials_json, const std::string& images_json, const std::string& textures_json,
                 const std::vector<uint32_t>& second_indices = {}) {
    std::vector<uint8_t> buffer;
    const auto append = [&](const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        buffer.insert(buffer.end(), bytes, bytes + size);
    };
    float min[3] = {1e9f, 1e9f, 1e9f};
    float max[3] = {-1e9f, -1e9f, -1e9f};
    for (const Vertex& vertex : vertices) {
        append(vertex.position, 12);
        for (int axis = 0; axis < 3; ++axis) {
            min[axis] = std::min(min[axis], vertex.position[axis]);
            max[axis] = std::max(max[axis], vertex.position[axis]);
        }
    }
    for (const Vertex& vertex : vertices) {
        append(vertex.normal, 12);
    }
    for (const Vertex& vertex : vertices) {
        append(vertex.uv, 8);
    }
    append(indices.data(), indices.size() * 4);
    const size_t count = vertices.size();
    std::string second_primitive;
    std::string second_view;
    std::string second_accessor;
    if (!second_indices.empty()) {
        const size_t offset = buffer.size();
        append(second_indices.data(), second_indices.size() * 4);
        second_primitive =
            R"(, {"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 4, "material": 1})";
        second_view = R"(, {"buffer": 0, "byteOffset": )" + std::to_string(offset) + R"(, "byteLength": )" +
                      std::to_string(second_indices.size() * 4) + "}";
        second_accessor = R"(, {"bufferView": 4, "componentType": 5125, "count": )" +
                          std::to_string(second_indices.size()) + R"(, "type": "SCALAR"})";
    }
    char text[4096];
    std::snprintf(text, sizeof(text),
                  R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0, "translation": [0, %g, 0]}],
  "meshes": [{"primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}%s]}],
  "buffers": [{"byteLength": %zu, "uri": "data:application/octet-stream;base64,%%s"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": %zu}, {"buffer": 0, "byteOffset": %zu, "byteLength": %zu},
                  {"buffer": 0, "byteOffset": %zu, "byteLength": %zu}, {"buffer": 0, "byteOffset": %zu, "byteLength": %zu}%s],
  "accessors": [
    {"bufferView": 0, "componentType": 5126, "count": %zu, "type": "VEC3", "min": [%g, %g, %g], "max": [%g, %g, %g]},
    {"bufferView": 1, "componentType": 5126, "count": %zu, "type": "VEC3"},
    {"bufferView": 2, "componentType": 5126, "count": %zu, "type": "VEC2"},
    {"bufferView": 3, "componentType": 5125, "count": %zu, "type": "SCALAR"}%s],
  "materials": %s, "images": %s, "textures": %s}
)",
                  node_y, second_primitive.c_str(), buffer.size(), count * 12, count * 12, count * 12, count * 24,
                  count * 8, count * 32, indices.size() * 4, second_view.c_str(), count, min[0], min[1], min[2], max[0],
                  max[1], max[2], count, count, indices.size(), second_accessor.c_str(), materials_json.c_str(),
                  images_json.c_str(), textures_json.c_str());
    std::string json(text);
    const size_t at = json.find("%s");
    json.replace(at, 2, base64(buffer.data(), buffer.size()));
    return json;
}

// A flat square, two triangles, facing +Z.
void quad(std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
    vertices = {{.position = {-1, -1, 0}, .normal = {0, 0, 1}, .uv = {0, 1}},
                {.position = {1, -1, 0}, .normal = {0, 0, 1}, .uv = {1, 1}},
                {.position = {1, 1, 0}, .normal = {0, 0, 1}, .uv = {1, 0}},
                {.position = {-1, 1, 0}, .normal = {0, 0, 1}, .uv = {0, 0}}};
    indices = {0, 1, 2, 0, 2, 3};
}

// A wavy n x n grid: something to simplify.
void grid(uint32_t n, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
    vertices.clear();
    indices.clear();
    for (uint32_t y = 0; y <= n; ++y) {
        for (uint32_t x = 0; x <= n; ++x) {
            const float u = static_cast<float>(x) / static_cast<float>(n);
            const float v = static_cast<float>(y) / static_cast<float>(n);
            vertices.push_back({.position = {u * 4.0f, 0.05f * std::sin(u * 6.0f) * std::cos(v * 5.0f), v * 4.0f},
                                .normal = {0, 1, 0},
                                .uv = {u, v}});
        }
    }
    for (uint32_t y = 0; y < n; ++y) {
        for (uint32_t x = 0; x < n; ++x) {
            const uint32_t i = y * (n + 1) + x;
            indices.insert(indices.end(), {i, i + n + 1, i + 1, i + 1, i + n + 1, i + n + 2});
        }
    }
}

// --- Reading results --------------------------------------------------------------------------

// Peak signal-to-noise ratio of decoded blocks against the source, over the given channels.
double psnr(const std::vector<uint8_t>& source, uint32_t size, std::span<const std::byte> blocks, TextureFormat format,
            int channels) {
    double error = 0.0;
    const uint32_t blocks_x = size / 4;
    for (uint32_t by = 0; by < size / 4; ++by) {
        for (uint32_t bx = 0; bx < blocks_x; ++bx) {
            uint8_t decoded[16 * 4] = {};
            const std::byte* block = blocks.data() + (size_t{by} * blocks_x + bx) * texture_block_bytes(format);
            if (format == TextureFormat::BC5Unorm) {
                rgbcx::unpack_bc5(block, decoded, 0, 1, 4);
            } else {
                REQUIRE(bc7decomp::unpack_bc7(block, reinterpret_cast<bc7decomp::color_rgba*>(decoded)));
            }
            for (uint32_t t = 0; t < 16; ++t) {
                const uint32_t x = bx * 4 + t % 4;
                const uint32_t y = by * 4 + t / 4;
                for (int c = 0; c < channels; ++c) {
                    const double d = static_cast<double>(decoded[t * 4 + c]) - source[(size_t{y} * size + x) * 4 + c];
                    error += d * d;
                }
            }
        }
    }
    const double mse = error / (static_cast<double>(size) * size * channels);
    return mse == 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
}

// A small scene: a quad with a color and a normal texture beside it, an unreferenced mask
// texture, and a folder rule.
struct Scene {
    std::vector<uint8_t> color = color_texels(64, 0);
    std::vector<uint8_t> normal = normal_texels(64);

    explicit Scene(const Sandbox& sandbox) {
        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;
        quad(vertices, indices);
        write_src(sandbox, "content/wall/wall.gltf",
                  gltf(vertices, indices, 1.0f,
                       R"([{"name": "brick", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0},
                             "baseColorFactor": [1, 0.5, 0.25, 1], "roughnessFactor": 0.75},
                           "normalTexture": {"index": 1}}])",
                       R"([{"uri": "bricks.png"}, {"uri": "bricks_n.png"}])", R"([{"source": 0}, {"source": 1}])"));
        write_src(sandbox, "content/wall/bricks.png", png(64, 64, color));
        write_src(sandbox, "content/wall/bricks_n.png", png(64, 64, normal));
        write_src(sandbox, "content/wall/dirt_mask.png", png(16, 16, color_texels(16, 3)));
        write_src(sandbox, "meta/_folder.json",
                  R"({"rules": [{"match": "*_mask.png", "texture": {"compression": "bc4", "srgb": false}}]})");
    }
};

} // namespace

TEST_CASE("cooking a source tree: metas, IDs, and formats picked by use and by folder rules") {
    const Sandbox sandbox;
    const Scene scene(sandbox);
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    CHECK(report.ok());
    CHECK(report.stats.cooked == 4);
    CHECK(report.stats.metas_created == 4);

    // Every source got a meta with a GUID; IDs derive from it.
    const AssetId wall = asset_id(guid_of(vfs, "src:/meta/wall/wall.gltf.json"));
    const AssetId bricks = asset_id(guid_of(vfs, "src:/meta/wall/bricks.png.json"));
    const AssetId bricks_n = asset_id(guid_of(vfs, "src:/meta/wall/bricks_n.png.json"));
    const AssetId mask = asset_id(guid_of(vfs, "src:/meta/wall/dirt_mask.png.json"));

    // The mesh: four vertices once merged, two triangles, raised 1 m by its node, one material slot.
    Cooked mesh_file(vfs, wall, AssetType::Mesh);
    const auto& mesh = mesh_file.load<MeshAsset>();
    CHECK(mesh.vertices.count == 4);
    CHECK(mesh.indices.count == 6);
    REQUIRE(mesh.lods.count >= 1);
    CHECK(mesh.lods.get(mesh_file.data())[0].part_count == 1);
    CHECK(mesh.bounds.min[1] == doctest::Approx(0.0f));
    CHECK(mesh.bounds.max[1] == doctest::Approx(2.0f));
    CHECK(mesh.bounds.radius == doctest::Approx(std::sqrt(2.0f)));
    REQUIRE(mesh.materials.count == 1);
    const MeshMaterialSlot& slot = mesh.materials.get(mesh_file.data())[0];
    CHECK(slot.material == sub_asset_id(wall, "material/brick"));
    CHECK(slot.name == de::core::StringId("brick", 5));
    const MeshVertex& corner = mesh.vertices.get(mesh_file.data())[0];
    float normal[3];
    de::cooker::decode_octahedral(corner.normal, normal);
    CHECK(normal[2] == doctest::Approx(1.0f).epsilon(0.001));

    // Its material: the glTF's factors, and the textures by their own IDs.
    Cooked material_file(vfs, slot.material, AssetType::Material);
    const auto& material = material_file.load<MaterialAsset>();
    CHECK(material.shader_template == material::kPbrOpaque);
    CHECK(material.find_texture(material_file.data(), material::kBaseColorTexture) == bricks);
    CHECK(material.find_texture(material_file.data(), material::kNormalTexture) == bricks_n);
    const MaterialParam* roughness = material.find_param(material_file.data(), material::kRoughness);
    REQUIRE(roughness != nullptr);
    CHECK(roughness->value[0] == 0.75f);
    CHECK(material.header.dependencies.count == 2);

    // Textures: color as sRGB BC7, the normal map as BC5 (from how the material uses them), the
    // mask as BC4 (folder rule). Full mip chains.
    Cooked color_file(vfs, bricks, AssetType::Texture);
    const auto& color = color_file.load<TextureAsset>();
    CHECK(color.format == TextureFormat::BC7Srgb);
    CHECK(color.mips.count == 7);
    CHECK(psnr(scene.color, 64, color.mip_data(color_file.data(), 0), color.format, 3) > 38.0);
    Cooked normal_file(vfs, bricks_n, AssetType::Texture);
    const auto& normal_map = normal_file.load<TextureAsset>();
    CHECK(normal_map.format == TextureFormat::BC5Unorm);
    CHECK((normal_map.flags & TextureFlags::NormalMap) == TextureFlags::NormalMap);
    CHECK(psnr(scene.normal, 64, normal_map.mip_data(normal_file.data(), 0), normal_map.format, 2) > 38.0);
    Cooked mask_file(vfs, mask, AssetType::Texture);
    const auto& mask_texture = mask_file.load<TextureAsset>();
    CHECK(mask_texture.format == TextureFormat::BC4Unorm);
    CHECK(mask_texture.mips.count == 5);
    const TextureMip& last = mask_texture.mips.get(mask_file.data())[4];
    CHECK(last.width == 1);
    CHECK(last.size == 8); // one block
}

TEST_CASE("a second cook skips what didn't change, and re-cooks what did") {
    const Sandbox sandbox;
    const Scene scene(sandbox);
    Vfs vfs = cooker_vfs(sandbox);
    REQUIRE(run_cook(vfs).ok());

    CookReport again = run_cook(vfs);
    CHECK(again.ok());
    CHECK(again.stats.cooked == 0);
    CHECK(again.stats.up_to_date == 4);

    // New texels: that texture only.
    write_src(sandbox, "content/wall/bricks.png", png(64, 64, color_texels(64, 9)));
    again = run_cook(vfs);
    CHECK(again.stats.cooked == 1);
    CHECK(again.stats.up_to_date == 3);

    // A setting in a meta: that texture only, now uncompressed.
    const Guid mask = guid_of(vfs, "src:/meta/wall/dirt_mask.png.json");
    write_src(sandbox, "meta/wall/dirt_mask.png.json",
              std::string(R"({"guid": ")") + format_guid(mask).c_str() + R"(", "texture": {"compression": "none"}})");
    again = run_cook(vfs);
    CHECK(again.stats.cooked == 1);
    Cooked mask_file(vfs, asset_id(mask), AssetType::Texture);
    CHECK(mask_file.load<TextureAsset>().format == TextureFormat::RGBA8Unorm);

    // A deleted cooked file is made again; --force cooks everything.
    REQUIRE(vfs.remove_file(cooked_path(asset_id(mask), AssetType::Texture).c_str()).ok());
    CHECK(run_cook(vfs).stats.cooked == 1);
    CHECK(run_cook(vfs, {.force = true}).stats.cooked == 4);

    // A new texture nothing uses: only it cooks (models depend on the textures they reference, not
    // on every texture there is).
    write_src(sandbox, "content/other/sky.png", png(8, 8, color_texels(8, 2)));
    again = run_cook(vfs);
    CHECK(again.stats.cooked == 1);
    CHECK(again.stats.up_to_date == 4);

    // A broken edit to the model: it fails, but its normal map keeps its use (BC5), and stays cooked.
    write_src(sandbox, "content/wall/wall.gltf", "{ not json");
    again = run_cook(vfs);
    CHECK(again.stats.failed == 1);
    CHECK(again.stats.cooked == 0);
    Cooked normal_file(vfs, asset_id(guid_of(vfs, "src:/meta/wall/bricks_n.png.json")), AssetType::Texture);
    CHECK(normal_file.load<TextureAsset>().format == TextureFormat::BC5Unorm);
}

TEST_CASE("metas follow files moved outside the tools; orphans, copies and bad names are reported") {
    const Sandbox sandbox;
    write_src(sandbox, "content/a/rock.png", png(8, 8, color_texels(8, 1)));
    write_src(sandbox, "content/a/moss.png", png(8, 8, color_texels(8, 2)));
    Vfs vfs = cooker_vfs(sandbox);
    REQUIRE(run_cook(vfs).ok());
    const Guid rock = guid_of(vfs, "src:/meta/a/rock.png.json");

    // Moved by hand: the meta follows by content, so the ID stays.
    write_src(sandbox, "content/b/rock.png", png(8, 8, color_texels(8, 1)));
    remove_src(sandbox, "content/a/rock.png");
    CookReport report = run_cook(vfs);
    CHECK(report.ok());
    CHECK(report.stats.metas_relinked == 1);
    CHECK(report.stats.metas_created == 0);
    CHECK_FALSE(vfs.exists("src:/meta/a/rock.png.json"));
    CHECK(guid_of(vfs, "src:/meta/b/rock.png.json") == rock);
    CHECK(vfs.exists(cooked_path(asset_id(rock), AssetType::Texture).c_str()));

    // Gone for good: the orphaned meta is reported, the cooked file removed, and a new file of
    // the same type gets no GUID until the orphan is settled.
    const Guid moss = guid_of(vfs, "src:/meta/a/moss.png.json");
    remove_src(sandbox, "content/a/moss.png");
    write_src(sandbox, "content/a/fern.png", png(8, 8, color_texels(8, 5)));
    report = run_cook(vfs);
    CHECK_FALSE(report.ok());
    CHECK(has_message(report, "meta/a/moss.png.json", "has no source file"));
    CHECK(has_message(report, "content/a/fern.png", "no GUID minted"));
    CHECK_FALSE(vfs.exists("src:/meta/a/fern.png.json"));
    CHECK(report.stats.removed == 1);
    CHECK_FALSE(vfs.exists(cooked_path(asset_id(moss), AssetType::Texture).c_str()));
    remove_src(sandbox, "meta/a/moss.png.json");
    report = run_cook(vfs);
    CHECK(report.ok());
    CHECK(report.stats.metas_created == 1);

    // A copied meta: two files, one GUID. The copy sorts first ("a/twin" < "b/rock"), yet the
    // original keeps its GUID and its cooked file (the last cook knew it by that GUID), and the copy
    // is the one reported.
    const CookedPath rock_file = cooked_path(asset_id(rock), AssetType::Texture);
    const std::string rock_bytes = read_text(vfs, rock_file.c_str());
    write_src(sandbox, "content/a/twin.png", png(8, 8, color_texels(8, 7)));
    write_src(sandbox, "meta/a/twin.png.json", read_text(vfs, "src:/meta/b/rock.png.json"));
    report = run_cook(vfs);
    CHECK(has_message(report, "content/a/twin.png", "same GUID as content/b/rock.png"));
    CHECK_FALSE(has_message(report, "content/b/rock.png", "same GUID"));
    CHECK(read_text(vfs, rock_file.c_str()) == rock_bytes);
    // Following the advice gives the copy a GUID of its own; the original's stays.
    remove_src(sandbox, "meta/a/twin.png.json");
    report = run_cook(vfs);
    CHECK(report.ok());
    CHECK(guid_of(vfs, "src:/meta/b/rock.png.json") == rock);
    CHECK(guid_of(vfs, "src:/meta/a/twin.png.json") != rock);

    // A broken meta (a merge conflict, say): reported, and what was cooked from it stays.
    const std::string fern_meta = read_text(vfs, "src:/meta/a/fern.png.json");
    const Guid fern = guid_of(vfs, "src:/meta/a/fern.png.json");
    write_src(sandbox, "meta/a/fern.png.json", "<<<<<<< HEAD\n" + fern_meta);
    report = run_cook(vfs);
    CHECK(report.stats.failed == 1);
    CHECK(report.stats.removed == 0);
    CHECK(vfs.exists(cooked_path(asset_id(fern), AssetType::Texture).c_str()));
    write_src(sandbox, "meta/a/fern.png.json", fern_meta);
    CHECK(run_cook(vfs).ok());

    // A meta deleted (as the message for copies says): the file gets a new GUID, cooks under its
    // new ID, and the old ID's file goes.
    remove_src(sandbox, "meta/a/fern.png.json");
    report = run_cook(vfs);
    CHECK(report.ok());
    const Guid new_fern = guid_of(vfs, "src:/meta/a/fern.png.json");
    REQUIRE(new_fern != fern);
    CHECK(report.stats.cooked == 1);
    CHECK(report.stats.removed == 1);
    CHECK(vfs.exists(cooked_path(asset_id(new_fern), AssetType::Texture).c_str()));
    CHECK_FALSE(vfs.exists(cooked_path(asset_id(fern), AssetType::Texture).c_str()));

    // A settings section that isn't an object would replace every default below it: refused.
    write_src(sandbox, "meta/a/fern.png.json",
              std::string(R"({"guid": ")") + format_guid(new_fern).c_str() + R"(", "texture": "bc5"})");
    report = run_cook(vfs);
    CHECK(has_message(report, "meta/a/fern.png.json", "should be an object"));
    CHECK(report.stats.failed == 1);
    write_src(sandbox, "meta/_folder.json", R"({"rules": [{"texture": {"compression": "bc4"}}]})");
    report = run_cook(vfs);
    CHECK(has_message(report, "meta/_folder.json", "needs a \"match\" pattern"));

    // Names that break the path rules.
    write_src(sandbox, "content/a/Big Rock.png", png(8, 8, color_texels(8, 1)));
    report = run_cook(vfs);
    CHECK(has_message(report, "content/a/Big Rock.png", "lowercase"));
}

TEST_CASE("--check reports what a cook would change and writes nothing") {
    const Sandbox sandbox;
    write_src(sandbox, "content/a/rock.png", png(8, 8, color_texels(8, 1)));
    Vfs vfs = cooker_vfs(sandbox, false);
    const CookReport report = run_cook(vfs, {.check_only = true});
    CHECK_FALSE(report.ok());
    CHECK(has_message(report, "content/a/rock.png", "has no meta"));
    CHECK_FALSE(vfs.exists("src:/meta/a/rock.png.json"));
    CHECK_FALSE(vfs.exists("game:/cook_state.json"));
}

TEST_CASE("meshes get a LOD chain over shared vertices, and embedded images cook with them") {
    const Sandbox sandbox;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    grid(48, vertices, indices);
    const std::vector<std::byte> glow = png(8, 8, color_texels(8, 4));
    const std::string glow_uri = "data:image/png;base64," + base64(glow.data(), glow.size());
    write_src(sandbox, "content/field/field.gltf",
              gltf(vertices, indices, 0.0f, R"([{"emissiveTexture": {"index": 0}, "emissiveFactor": [1, 1, 1]}])",
                   R"([{"uri": ")" + glow_uri + R"("}])", R"([{"source": 0}])"));
    write_src(sandbox, "meta/field/_folder.json", R"({"rules": [{"match": "*.gltf", "mesh": {"lod_count": 5}}]})");
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    REQUIRE(report.ok());
    const AssetId field = asset_id(guid_of(vfs, "src:/meta/field/field.gltf.json"));

    Cooked file(vfs, field, AssetType::Mesh);
    const auto& mesh = file.load<MeshAsset>();
    const auto lods = mesh.lods.get(file.data());
    const auto parts = mesh.parts.get(file.data());
    REQUIRE(lods.size() >= 3);
    CHECK(lods.size() <= 5);
    uint32_t previous = UINT32_MAX;
    for (size_t i = 0; i < lods.size(); ++i) {
        const MeshPart& part = parts[lods[i].first_part];
        CHECK(part.index_count < previous); // each level smaller
        previous = part.index_count;
        CHECK(lods[i].error >= (i == 0 ? 0.0f : lods[i - 1].error));
    }
    CHECK(lods[0].error == 0.0f);
    CHECK(parts[lods[0].first_part].index_count == 48 * 48 * 6);
    CHECK(mesh.vertices.count == 49 * 49); // shared by every level

    // The embedded image: a sub-asset of the model, cooked as color.
    const MeshMaterialSlot& slot = mesh.materials.get(file.data())[0];
    CHECK(slot.material == sub_asset_id(field, "material/#0")); // unnamed: by index
    Cooked material_file(vfs, slot.material, AssetType::Material);
    const auto& material = material_file.load<MaterialAsset>();
    const AssetId glow_id = material.find_texture(material_file.data(), material::kEmissiveTexture);
    CHECK(glow_id == sub_asset_id(field, "image/0"));
    Cooked glow_file(vfs, glow_id, AssetType::Texture);
    CHECK(glow_file.load<TextureAsset>().format == TextureFormat::BC7Srgb);
}

TEST_CASE("a source that fails keeps track of what it wrote, so nothing is left behind") {
    const Sandbox sandbox;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    quad(vertices, indices);
    const std::vector<std::byte> good = png(8, 8, color_texels(8, 4));
    const std::string good_uri = "data:image/png;base64," + base64(good.data(), good.size());
    // The first embedded image cooks; the second isn't an image, so the model fails after writing one file.
    write_src(sandbox, "content/broken/sign.gltf",
              gltf(vertices, indices, 0.0f,
                   R"([{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}, "emissiveTexture": {"index": 1}}])",
                   R"([{"uri": ")" + good_uri + R"("}, {"uri": "data:image/png;base64,AAAAAAAA"}])",
                   R"([{"source": 0}, {"source": 1}])"));
    write_src(sandbox, "content/broken/fine.png", png(8, 8, color_texels(8, 6)));
    Vfs vfs = cooker_vfs(sandbox);
    CookReport report = run_cook(vfs);
    CHECK_FALSE(report.ok());
    CHECK(report.stats.failed == 1);
    CHECK(report.stats.cooked == 1);
    const AssetId sign = asset_id(guid_of(vfs, "src:/meta/broken/sign.gltf.json"));
    const CookedPath image = cooked_path(sub_asset_id(sign, "image/0"), AssetType::Texture);
    CHECK(vfs.exists(image.c_str()));
    CHECK_FALSE(vfs.exists(cooked_path(sign, AssetType::Mesh).c_str()));

    // It fails again (and is tried again: failed sources are never up to date), while the rest of
    // the cook state survives it...
    report = run_cook(vfs);
    CHECK(report.stats.failed == 1);
    CHECK(report.stats.up_to_date == 1);
    // ...and once it's gone, so is what it wrote.
    remove_src(sandbox, "content/broken/sign.gltf");
    remove_src(sandbox, "meta/broken/sign.gltf.json");
    report = run_cook(vfs);
    CHECK(report.ok());
    CHECK(report.stats.removed == 1);
    CHECK_FALSE(vfs.exists(image.c_str()));
}

TEST_CASE("FBX converts to meters and Y up, with its materials") {
    const Sandbox sandbox;
    // A square of 200 x 200 centimeters in the XY plane (FBX's default unit is the centimeter).
    write_src(sandbox, "content/props/sign.fbx", R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",1
	}
}
Objects:  {
	Geometry: 1000, "Geometry::sign", "Mesh" {
		Vertices: *12 {
			a: -100,-100,0,100,-100,0,100,100,0,-100,100,0
		}
		PolygonVertexIndex: *4 {
			a: 0,1,2,-4
		}
		GeometryVersion: 124
		LayerElementMaterial: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "AllSame"
			ReferenceInformationType: "IndexToDirect"
			Materials: *1 {
				a: 0
			}
		}
		Layer: 0 {
			Version: 100
			LayerElement:  {
				Type: "LayerElementMaterial"
				TypedIndex: 0
			}
		}
	}
	Model: 2000, "Model::sign", "Mesh" {
		Version: 232
	}
	Material: 3000, "Material::paint", "" {
		Version: 102
		ShadingModel: "lambert"
		Properties70:  {
			P: "DiffuseColor", "Color", "", "A",0.8,0.2,0.1
		}
	}
}
Connections:  {
	C: "OO",1000,2000
	C: "OO",2000,0
	C: "OO",3000,2000
}
)");
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    REQUIRE(report.ok());
    const AssetId sign = asset_id(guid_of(vfs, "src:/meta/props/sign.fbx.json"));
    Cooked file(vfs, sign, AssetType::Mesh);
    const auto& mesh = file.load<MeshAsset>();
    CHECK(mesh.indices.count == 6); // the quad, triangulated
    CHECK(mesh.vertices.count == 4);
    CHECK(mesh.bounds.min[0] == doctest::Approx(-1.0f));
    CHECK(mesh.bounds.max[0] == doctest::Approx(1.0f));
    CHECK(mesh.bounds.max[1] == doctest::Approx(1.0f));
    const MeshMaterialSlot& slot = mesh.materials.get(file.data())[0];
    CHECK(slot.material == sub_asset_id(sign, "material/paint"));
    Cooked material_file(vfs, slot.material, AssetType::Material);
    const auto& material = material_file.load<MaterialAsset>();
    const MaterialParam* color = material.find_param(material_file.data(), material::kBaseColor);
    REQUIRE(color != nullptr);
    CHECK(color->value[0] == doctest::Approx(0.8f));
    CHECK(color->value[1] == doctest::Approx(0.2f));
}

TEST_CASE("a malformed embedded image is reported, not trusted") {
    const Sandbox sandbox;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    quad(vertices, indices);
    // Only padding: a size computed from it would wrap around.
    write_src(sandbox, "content/broken/bad.gltf",
              gltf(vertices, indices, 0.0f, R"([{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}])",
                   R"([{"uri": "data:image/png;base64,=="}])", R"([{"source": 0}])"));
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    CHECK(report.stats.failed == 1);
    CHECK(has_message(report, "content/broken/bad.gltf", "isn't valid base64"));
}

TEST_CASE("a mesh with its vertices split all over still gets a long LOD chain, with errors that only rise") {
    const Sandbox sandbox;
    std::vector<Vertex> welded;
    std::vector<uint32_t> welded_indices;
    grid(48, welded, welded_indices);
    // Every triangle has vertices of its own, kept apart by its uvs, as in a scan with its uvs in pieces.
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    for (size_t i = 0; i < welded_indices.size(); ++i) {
        Vertex vertex = welded[welded_indices[i]];
        const size_t triangle = i / 3;
        vertex.uv[0] += 0.001f * static_cast<float>(triangle);
        indices.push_back(static_cast<uint32_t>(vertices.size()));
        vertices.push_back(vertex);
    }
    write_src(sandbox, "content/split/split.gltf", gltf(vertices, indices, 0.0f, R"([{}])", "[]", "[]"));
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    REQUIRE(report.ok());
    const AssetId split = asset_id(guid_of(vfs, "src:/meta/split/split.gltf.json"));

    Cooked file(vfs, split, AssetType::Mesh);
    const auto& mesh = file.load<MeshAsset>();
    const auto lods = mesh.lods.get(file.data());
    const auto parts = mesh.parts.get(file.data());
    CHECK(lods.size() >= 6); // what the default setting allows, this far
    CHECK(lods.size() <= 8);
    for (size_t i = 1; i < lods.size(); ++i) {
        CAPTURE(i);
        CHECK(lods[i].error >= lods[i - 1].error);
        CHECK(parts[lods[i].first_part].index_count < parts[lods[i - 1].first_part].index_count * 3 / 4);
    }
}

TEST_CASE("a mesh that simplifies well keeps its uv seams in every LOD") {
    const Sandbox sandbox;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    grid(48, vertices, indices);
    // A seam down the middle: each side has its own vertices along it, with uvs far apart.
    constexpr uint32_t kN = 48;
    std::vector<Vertex> split_vertices;
    std::vector<uint32_t> split_indices;
    const auto add_side = [&](uint32_t first_x, uint32_t end_x, float uv_offset) {
        const auto first_vertex = static_cast<uint32_t>(split_vertices.size());
        for (uint32_t y = 0; y <= kN; ++y) {
            for (uint32_t x = first_x; x <= end_x; ++x) {
                Vertex vertex = vertices[(y * (kN + 1)) + x];
                vertex.uv[0] += uv_offset;
                split_vertices.push_back(vertex);
            }
        }
        const uint32_t width = end_x - first_x + 1;
        for (uint32_t y = 0; y < kN; ++y) {
            for (uint32_t x = 0; x + 1 < width; ++x) {
                const uint32_t i = first_vertex + (y * width) + x;
                for (const uint32_t index : {i, i + width, i + 1, i + 1, i + width, i + width + 1}) {
                    split_indices.push_back(index);
                }
            }
        }
    };
    add_side(0, kN / 2, 0.0f);
    add_side(kN / 2, kN, 10.0f);
    write_src(sandbox, "content/seam/seam.gltf", gltf(split_vertices, split_indices, 0.0f, R"([{}])", "[]", "[]"));
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    REQUIRE(report.ok());
    const AssetId seam = asset_id(guid_of(vfs, "src:/meta/seam/seam.gltf.json"));

    Cooked file(vfs, seam, AssetType::Mesh);
    const auto& mesh = file.load<MeshAsset>();
    const auto lods = mesh.lods.get(file.data());
    const auto parts = mesh.parts.get(file.data());
    const auto mesh_vertices = mesh.vertices.get(file.data());
    const auto mesh_indices = mesh.indices.get(file.data());
    REQUIRE(lods.size() >= 4);
    for (size_t l = 0; l < 4; ++l) {
        const MeshPart& part = parts[lods[l].first_part];
        uint32_t crossing = 0;
        for (uint32_t i = 0; i < part.index_count; i += 3) {
            const auto side = [&](uint32_t corner) {
                return mesh_vertices[mesh_indices[part.first_index + i + corner]].uv[0] > 5.0f;
            };
            crossing += side(0) != side(1) || side(1) != side(2) ? 1 : 0;
        }
        CAPTURE(l);
        CHECK(crossing == 0); // a triangle across it would smear the whole texture over itself
    }
}

TEST_CASE("a flat mesh's LODs have an error for the colors and uvs they lose") {
    de::cooker::ModelData model;
    de::cooker::ModelPrimitive& primitive = model.primitives.emplace();
    primitive.material = 0;
    constexpr uint32_t kCells = 32;
    for (uint32_t y = 0; y <= kCells; ++y) {
        for (uint32_t x = 0; x <= kCells; ++x) {
            const float u = static_cast<float>(x) / kCells;
            const float v = static_cast<float>(y) / kCells;
            de::cooker::ModelVertex& vertex = primitive.vertices.emplace();
            vertex.position[0] = u;
            vertex.position[2] = v; // flat, so every LOD has the same shape
            vertex.color[0] = 0.5f + (0.5f * std::sin(u * 40.0f) * std::sin(v * 30.0f)); // painted
        }
    }
    for (uint32_t y = 0; y < kCells; ++y) {
        for (uint32_t x = 0; x < kCells; ++x) {
            const uint32_t a = (y * (kCells + 1)) + x;
            const uint32_t b = a + kCells + 1;
            for (const uint32_t index : {a, b, a + 1, a + 1, b, b + 1}) {
                primitive.indices.push(index);
            }
        }
    }
    static_cast<void>(model.materials.emplace());
    de::cooker::Diagnostics diagnostics;
    const AssetId materials[] = {AssetId::from_value(0x1)};
    const auto cooked =
        de::cooker::cook_mesh(model, materials, {.lod_count = 4}, AssetId::from_value(0x2), "test", diagnostics);
    REQUIRE(cooked.ok());
    const auto& mesh = *reinterpret_cast<const MeshAsset*>(cooked->data());
    const auto lods = mesh.lods.get(cooked->data());
    REQUIRE(lods.size() >= 2);
    CHECK(lods[1].error > 0.0f);
    CHECK(lods[lods.size() - 1].error > 0.01f); // a real share of its size, so it's only picked from far enough away
}

TEST_CASE("a part simplified away counts as that much error") {
    // A big bumpy grid, and a tiny part of another material far off to its side, that simplifies to nothing.
    de::cooker::ModelData model;
    de::cooker::ModelPrimitive& big = model.primitives.emplace();
    big.material = 0;
    constexpr uint32_t kCells = 32;
    for (uint32_t y = 0; y <= kCells; ++y) {
        for (uint32_t x = 0; x <= kCells; ++x) {
            const float u = static_cast<float>(x) / kCells;
            const float v = static_cast<float>(y) / kCells;
            de::cooker::ModelVertex& vertex = big.vertices.emplace();
            vertex.position[0] = u * 4.0f;
            vertex.position[1] = 0.1f * std::sin(u * 9.0f) * std::sin(v * 7.0f);
            vertex.position[2] = v * 4.0f;
        }
    }
    for (uint32_t y = 0; y < kCells; ++y) {
        for (uint32_t x = 0; x < kCells; ++x) {
            const uint32_t a = (y * (kCells + 1)) + x;
            const uint32_t b = a + kCells + 1;
            for (const uint32_t index : {a, b, a + 1, a + 1, b, b + 1}) {
                big.indices.push(index);
            }
        }
    }
    de::cooker::ModelPrimitive& small = model.primitives.emplace();
    small.material = 1;
    constexpr float kSmallSize = 0.2f;
    const float corners[4][3] = {{10, 0, 0}, {10 + kSmallSize, 0, 0}, {10, kSmallSize, 0}, {10, 0, kSmallSize}};
    for (const auto& corner : corners) {
        de::cooker::ModelVertex& vertex = small.vertices.emplace();
        std::copy_n(corner, 3, vertex.position);
    }
    for (const uint32_t index : {0U, 2U, 1U, 0U, 1U, 3U, 0U, 3U, 2U, 1U, 2U, 3U}) {
        small.indices.push(index);
    }
    static_cast<void>(model.materials.emplace());
    static_cast<void>(model.materials.emplace());
    de::cooker::Diagnostics diagnostics;
    const AssetId materials[] = {AssetId::from_value(0x1), AssetId::from_value(0x2)};
    const auto cooked =
        de::cooker::cook_mesh(model, materials, {.lod_count = 8}, AssetId::from_value(0x3), "test", diagnostics);
    REQUIRE(cooked.ok());
    const auto& mesh = *reinterpret_cast<const MeshAsset*>(cooked->data());
    const auto lods = mesh.lods.get(cooked->data());
    bool any_gone = false;
    for (size_t i = 1; i < lods.size(); ++i) {
        CAPTURE(i);
        if (lods[i].part_count < lods[0].part_count) {
            any_gone = true;
            CHECK(lods[i].error >= kSmallSize);
        }
    }
    MESSAGE("a part was simplified away: ", any_gone);
}

TEST_CASE("LODs keep material seams closed") {
    const Sandbox sandbox;
    // One wavy grid, its left half in one material and its right half in another: the halves
    // share the column of vertices at x = 2 m.
    constexpr uint32_t kSize = 32;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> all;
    grid(kSize, vertices, all);
    std::vector<uint32_t> left;
    std::vector<uint32_t> right;
    for (size_t t = 0; t < all.size(); t += 3) {
        const bool is_left = (t / 6) % kSize < kSize / 2; // two triangles per cell, kSize cells a row
        (is_left ? left : right)
            .insert((is_left ? left : right).end(), all.begin() + static_cast<std::ptrdiff_t>(t),
                    all.begin() + static_cast<std::ptrdiff_t>(t + 3));
    }
    write_src(sandbox, "content/field/split.gltf",
              gltf(vertices, left, 0.0f, R"([{"name": "grass"}, {"name": "dirt"}])", "[]", "[]", right));
    Vfs vfs = cooker_vfs(sandbox);
    REQUIRE(run_cook(vfs).ok());
    Cooked file(vfs, asset_id(guid_of(vfs, "src:/meta/field/split.gltf.json")), AssetType::Mesh);
    const auto& mesh = file.load<MeshAsset>();
    const auto lods = mesh.lods.get(file.data());
    const auto parts = mesh.parts.get(file.data());
    const auto indices = mesh.indices.get(file.data());
    const auto mesh_vertices = mesh.vertices.get(file.data());
    REQUIRE(lods.size() >= 2);
    for (const MeshLod& lod : lods) {
        REQUIRE(lod.part_count == 2);
        // Each part's vertices on the seam: every one of the column, in both parts, at every LOD.
        for (uint32_t p = 0; p < 2; ++p) {
            const MeshPart& part = parts[lod.first_part + p];
            std::vector<float> seam;
            for (uint32_t i = 0; i < part.index_count; ++i) {
                const MeshVertex& vertex = mesh_vertices[indices[part.first_index + i]];
                if (std::abs(vertex.position[0] - 2.0f) < 1e-4f &&
                    std::ranges::find(seam, vertex.position[2]) == seam.end()) {
                    seam.push_back(vertex.position[2]);
                }
            }
            CHECK(seam.size() == kSize + 1);
        }
    }
    CHECK(parts[lods.back().first_part].index_count < parts[0].index_count); // and it still simplified
}

TEST_CASE("FBX tangents keep their handedness under a mirroring node") {
    const Sandbox sandbox;
    // A quad with tangents along +U and binormals along +V, in a node mirrored in X. In the engine's
    // space (V flipped, then mirrored) the bitangent cross(n, t) * w points to -Y: w = +1.
    std::string fbx = R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UpAxis", "int", "Integer", "",1
		P: "UpAxisSign", "int", "Integer", "",1
		P: "FrontAxis", "int", "Integer", "",2
		P: "FrontAxisSign", "int", "Integer", "",1
		P: "CoordAxis", "int", "Integer", "",0
		P: "CoordAxisSign", "int", "Integer", "",1
		P: "UnitScaleFactor", "double", "Number", "",100
	}
}
Objects:  {
	Geometry: 1000, "Geometry::card", "Mesh" {
		Vertices: *12 {
			a: 0,0,0,1,0,0,1,1,0,0,1,0
		}
		PolygonVertexIndex: *4 {
			a: 0,1,2,-4
		}
		GeometryVersion: 124
		LayerElementNormal: 0 {
			Version: 101
			Name: ""
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "Direct"
			Normals: *12 {
				a: 0,0,1,0,0,1,0,0,1,0,0,1
			}
		}
		LayerElementBinormal: 0 {
			Version: 101
			Name: "map1"
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "Direct"
			Binormals: *12 {
				a: 0,1,0,0,1,0,0,1,0,0,1,0
			}
		}
		LayerElementTangent: 0 {
			Version: 101
			Name: "map1"
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "Direct"
			Tangents: *12 {
				a: 1,0,0,1,0,0,1,0,0,1,0,0
			}
		}
		LayerElementUV: 0 {
			Version: 101
			Name: "map1"
			MappingInformationType: "ByPolygonVertex"
			ReferenceInformationType: "IndexToDirect"
			UV: *8 {
				a: 0,0,1,0,1,1,0,1
			}
			UVIndex: *4 {
				a: 0,1,2,3
			}
		}
		Layer: 0 {
			Version: 100
			LayerElement:  {
				Type: "LayerElementNormal"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementBinormal"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementTangent"
				TypedIndex: 0
			}
			LayerElement:  {
				Type: "LayerElementUV"
				TypedIndex: 0
			}
		}
	}
	Model: 2000, "Model::card", "Mesh" {
		Version: 232
		Properties70:  {
			P: "Lcl Scaling", "Lcl Scaling", "", "A",MIRROR,1,1
		}
	}
}
Connections:  {
	C: "OO",1000,2000
	C: "OO",2000,0
}
)";
    const auto tangent_sign = [&](const char* name, const char* mirror) {
        std::string text = fbx;
        text.replace(text.find("MIRROR"), 6, mirror);
        const std::string path = std::string("content/props/") + name;
        write_src(sandbox, path, text);
        Vfs vfs = cooker_vfs(sandbox);
        REQUIRE(run_cook(vfs).ok());
        Cooked file(vfs, asset_id(guid_of(vfs, ("src:/meta/props/" + std::string(name) + ".json").c_str())),
                    AssetType::Mesh);
        const auto& mesh = file.load<MeshAsset>();
        int sign = 0;
        for (const MeshVertex& vertex : mesh.vertices.get(file.data())) {
            const auto w = static_cast<int8_t>(vertex.tangent >> 24U);
            CHECK((sign == 0 || sign == (w > 0 ? 1 : -1)));
            sign = w > 0 ? 1 : -1;
        }
        return sign;
    };
    CHECK(tangent_sign("plain.fbx", "1") == -1);
    CHECK(tangent_sign("mirrored.fbx", "-1") == 1);
}

TEST_CASE("embedded images follow how they're used, not rules meant for file names") {
    const Sandbox sandbox;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    quad(vertices, indices);
    const std::vector<std::byte> color = png(8, 8, color_texels(8, 4));
    const std::string color_uri = "data:image/png;base64," + base64(color.data(), color.size());
    // "gas_mask" matches the mask rule, which is for mask textures, not for a model's images.
    write_src(sandbox, "meta/_folder.json",
              R"({"rules": [{"match": "*_mask.*", "texture": {"compression": "bc4", "srgb": false}}]})");
    write_src(sandbox, "content/props/gas_mask.gltf",
              gltf(vertices, indices, 0.0f, R"([{"pbrMetallicRoughness": {"baseColorTexture": {"index": 0}}}])",
                   R"([{"uri": ")" + color_uri + R"("}])", R"([{"source": 0}])"));
    Vfs vfs = cooker_vfs(sandbox);
    REQUIRE(run_cook(vfs).ok());
    const Guid mask = guid_of(vfs, "src:/meta/props/gas_mask.gltf.json");
    Cooked image(vfs, sub_asset_id(asset_id(mask), "image/0"), AssetType::Texture);
    CHECK(image.load<TextureAsset>().format == TextureFormat::BC7Srgb);

    // A bad setting in the model's meta fails the model, every time, until it's fixed.
    write_src(sandbox, "meta/props/gas_mask.gltf.json",
              std::string(R"({"guid": ")") + format_guid(mask).c_str() + R"(", "texture": {"compression": "bc9"}})");
    CookReport report = run_cook(vfs);
    CHECK(report.stats.failed == 1);
    CHECK(has_message(report, "content/props/gas_mask.gltf", "bc9"));
    report = run_cook(vfs);
    CHECK(report.stats.failed == 1);
    CHECK_FALSE(report.ok());
}

TEST_CASE("data textures filter each channel on its own") {
    const Sandbox sandbox;
    // Columns of transparent black and opaque white. Color filtering weights by alpha (white
    // wins); data keeps channels apart (gray).
    std::vector<uint8_t> texels(size_t{4} * 4 * 4);
    for (uint32_t i = 0; i < 16; ++i) {
        const uint8_t value = (i % 4) % 2 == 0 ? 0 : 255;
        for (uint32_t c = 0; c < 4; ++c) {
            texels[i * 4 + c] = value;
        }
    }
    write_src(sandbox, "meta/_folder.json",
              R"({"rules": [{"match": "*_data.png", "texture": {"compression": "none", "srgb": false}},
                            {"match": "*_color.png", "texture": {"compression": "none"}}]})");
    write_src(sandbox, "content/a/packed_data.png", png(4, 4, texels));
    write_src(sandbox, "content/a/leaf_color.png", png(4, 4, texels));
    Vfs vfs = cooker_vfs(sandbox);
    REQUIRE(run_cook(vfs).ok());
    const auto mip1_red = [&](const char* meta) {
        Cooked file(vfs, asset_id(guid_of(vfs, meta)), AssetType::Texture);
        const auto& texture = file.load<TextureAsset>();
        return static_cast<int>(texture.mip_data(file.data(), 1)[0]);
    };
    const int data = mip1_red("src:/meta/a/packed_data.png.json");
    CHECK(data > 90);
    CHECK(data < 165);
    CHECK(mip1_red("src:/meta/a/leaf_color.png.json") > 240);
}

TEST_CASE("materials named like the index form don't collide with it") {
    const Sandbox sandbox;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    quad(vertices, indices);
    // Material 0 is named "#1", material 1 has no name: "material/#1" would be both.
    write_src(sandbox, "content/a/pair.gltf",
              gltf(vertices, {0, 1, 2}, 0.0f, R"([{"name": "#1"}, {}])", "[]", "[]", {0, 2, 3}));
    Vfs vfs = cooker_vfs(sandbox);
    REQUIRE(run_cook(vfs).ok());
    const AssetId pair = asset_id(guid_of(vfs, "src:/meta/a/pair.gltf.json"));
    Cooked file(vfs, pair, AssetType::Mesh);
    const auto slots = file.load<MeshAsset>().materials.get(file.data());
    REQUIRE(slots.size() == 2);
    CHECK(slots[0].material == sub_asset_id(pair, "material/#0"));
    CHECK(slots[1].material == sub_asset_id(pair, "material/#1"));
}

TEST_CASE("FBX textures are found by name when only an absolute path was saved") {
    const Sandbox sandbox;
    write_src(sandbox, "content/props/card_albedo.png", png(8, 8, color_texels(8, 3)));
    write_src(sandbox, "content/props/card.fbx", R"(; FBX 7.4.0 project file
FBXHeaderExtension:  {
	FBXHeaderVersion: 1003
	FBXVersion: 7400
}
GlobalSettings:  {
	Version: 1000
	Properties70:  {
		P: "UnitScaleFactor", "double", "Number", "",100
	}
}
Objects:  {
	Geometry: 1000, "Geometry::card", "Mesh" {
		Vertices: *12 {
			a: 0,0,0,1,0,0,1,1,0,0,1,0
		}
		PolygonVertexIndex: *4 {
			a: 0,1,2,-4
		}
		GeometryVersion: 124
	}
	Model: 2000, "Model::card", "Mesh" {
		Version: 232
	}
	Material: 3000, "Material::paint", "" {
		Version: 102
		ShadingModel: "lambert"
	}
	Texture: 4000, "Texture::albedo", "" {
		Type: "TextureVideoClip"
		Version: 202
		TextureName: "Texture::albedo"
		FileName: "C:/Users/artist/props/card_albedo.png"
		RelativeFilename: ""
	}
}
Connections:  {
	C: "OO",1000,2000
	C: "OO",2000,0
	C: "OO",3000,2000
	C: "OP",4000,3000, "DiffuseColor"
}
)");
    Vfs vfs = cooker_vfs(sandbox);
    REQUIRE(run_cook(vfs).ok());
    const AssetId card = asset_id(guid_of(vfs, "src:/meta/props/card.fbx.json"));
    Cooked file(vfs, sub_asset_id(card, "material/paint"), AssetType::Material);
    const auto& material = file.load<MaterialAsset>();
    CHECK(material.find_texture(file.data(), material::kBaseColorTexture) ==
          asset_id(guid_of(vfs, "src:/meta/props/card_albedo.png.json")));
}

TEST_CASE("modules with entry points are told from libraries however the attribute is spaced") {
    const auto defines = [](std::string_view text) {
        return de::cooker::defines_entry_points(std::as_bytes(std::span(text.data(), text.size())));
    };
    CHECK(defines(R"([shader("vertex")] float4 main() {})"));
    CHECK(defines(R"([ shader ( "compute" ) ] [numthreads(8, 1, 1)] void main() {})"));
    CHECK(defines("[numthreads(8, 1, 1)]\n[shader(\"compute\")]\nvoid main() {}"));
    CHECK_FALSE(defines(R"(float4 helper() { return 0; })"));
    CHECK_FALSE(defines("// call it from [shader(\"vertex\")] entry points\nfloat4 helper();"));
    CHECK_FALSE(defines("/* [shader(\"vertex\")] */ float4 helper();"));
    CHECK_FALSE(defines(R"([shaders("x")] float4 helper();)"));
}

TEST_CASE("text materials name their textures by path and set how they are used") {
    const Sandbox sandbox;
    write_src(sandbox, "content/rock/rock_albedo.png", png(8, 8, color_texels(8, 1)));
    write_src(sandbox, "content/rock/rock_nrm.png", png(8, 8, normal_texels(8)));
    write_src(sandbox, "content/rock/rock.material", R"({
  // comments are fine
  "alpha_mode": "masked",
  "double_sided": true,
  "params": {"base_color": [0.9, 0.8, 0.7], "alpha_cutoff": 0.3, "wetness": 0.5},
  "textures": {"base_color": "rock_albedo.png", "normal": "rock_nrm.png"},
})");
    write_src(sandbox, "content/rock/broken.material", R"({"textures": {"base_color": "missing.png"}, "colour": 1})");
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    CHECK(has_message(report, "content/rock/broken.material", "should name a texture source"));
    CHECK(has_message(report, "content/rock/broken.material", "unknown material setting 'colour'"));
    CHECK(report.stats.failed == 1);

    const AssetId rock = asset_id(guid_of(vfs, "src:/meta/rock/rock.material.json"));
    Cooked file(vfs, rock, AssetType::Material);
    const auto& material = file.load<MaterialAsset>();
    CHECK(material.alpha_mode == AlphaMode::Masked);
    CHECK(material.shader_template == material::kPbrMasked);
    CHECK((material.flags & MaterialFlags::DoubleSided) == MaterialFlags::DoubleSided);
    const MaterialParam* wetness = material.find_param(file.data(), de::core::StringId("wetness", 7));
    REQUIRE(wetness != nullptr);
    CHECK(wetness->value[0] == 0.5f);
    // An rgb color is opaque: w defaults to 1 (a masked material would otherwise vanish).
    const MaterialParam* base_color = material.find_param(file.data(), material::kBaseColor);
    REQUIRE(base_color != nullptr);
    CHECK(base_color->value[2] == doctest::Approx(0.7f));
    CHECK(base_color->value[3] == 1.0f);
    const AssetId normal = asset_id(guid_of(vfs, "src:/meta/rock/rock_nrm.png.json"));
    CHECK(material.find_texture(file.data(), material::kNormalTexture) == normal);
    Cooked normal_file(vfs, normal, AssetType::Texture);
    CHECK(normal_file.load<TextureAsset>().format == TextureFormat::BC5Unorm);
}

TEST_CASE("shader modules cook to .deshader, and the library builds pipelines from them") {
    const Sandbox sandbox;
    // src:/ is the repository: its real shaders.
    const de::platform::PathString game_root = sandbox.game();
    const de::platform::PathString user_root = sandbox.folder("user");
    const de::platform::PathString temp_root = sandbox.folder("temp");
    auto vfs = Vfs::create({.game_root = game_root.c_str(),
                            .user_root = user_root.c_str(),
                            .temp_root = temp_root.c_str(),
                            .src_root = DE_SOURCE_DIR,
                            .game_writable = true});
    REQUIRE(vfs.ok());
    auto compiler = de::shader::ShaderCompiler::create(*vfs, {.debug_info = false, .checks = false});
    REQUIRE(compiler.ok());
    de::core::JobSystem jobs({.worker_count = 1});
    const CookReport report = de::cooker::cook(*vfs, jobs, &*compiler, {.test_shaders = true, .content = false});
    // tests.own_slot binds a descriptor slot on purpose: refused, as the library refuses it.
    CHECK(has_message(report, "shaders/tests/own_slot.slang", "descriptor slot"));
    CHECK(report.stats.failed == 1);
    CHECK(report.stats.cooked >= 8);
    const CookReport again = de::cooker::cook(*vfs, jobs, &*compiler, {.test_shaders = true, .content = false});
    CHECK(again.stats.cooked == 0);
    CHECK(again.stats.up_to_date == report.stats.cooked);
    // Cooking without a compiler (--no-shaders) leaves cooked shaders alone.
    const CookReport content_only = de::cooker::cook(*vfs, jobs, nullptr, {.test_shaders = true, .content = false});
    CHECK(content_only.stats.removed == 0);
    CHECK(vfs->exists(cooked_path(shader_asset_id("tests.solid_color"), AssetType::Shader).c_str()));
    CHECK(de::cooker::cook(*vfs, jobs, &*compiler, {.test_shaders = true, .content = false}).stats.up_to_date ==
          report.stats.cooked);

    // The cooked module holds every entry point, as the compiler makes them.
    Cooked file(*vfs, shader_asset_id("tests.solid_color"), AssetType::Shader);
    const auto& shader = file.load<ShaderAsset>();
    CHECK((std::string_view(shader.module) == "tests.solid_color"));
    const ShaderPermutation* permutation = shader.find_permutation(file.data(), 0);
    REQUIRE(permutation != nullptr);
    const ShaderEntryCode* fragment = ShaderAsset::find_entry(file.data(), *permutation, "fragment_main");
    REQUIRE(fragment != nullptr);
    CHECK(fragment->stage == ShaderStageKind::Fragment);
    CHECK(fragment->push_constant_bytes == 16);
    const de::shader::ShaderEntry entries[] = {{.name = "vertex_main", .stage = de::rhi::ShaderStage::Vertex},
                                               {.name = "fragment_main", .stage = de::rhi::ShaderStage::Fragment}};
    const auto compiled = compiler->compile({.module = "tests.solid_color", .entries = entries});
    REQUIRE(compiled.ok());
    const auto cooked_code = fragment->code.get(file.data());
    REQUIRE(cooked_code.size() == compiled->stages[1].code.size());
    CHECK(std::memcmp(cooked_code.data(), compiled->stages[1].code.data(), cooked_code.size_bytes()) == 0);
    CHECK_FALSE(vfs->exists(cooked_path(shader_asset_id("common.color"), AssetType::Shader).c_str())); // a library

    // A library over the cooked files builds the same programs, and fails cleanly on what isn't cooked.
    auto device = de::test::make_device();
    if (!device) {
        return;
    }
    auto library = de::shader::ShaderLibrary::create_cooked(*device, *vfs, jobs, {.hot_reload = false});
    REQUIRE(library.ok());
    de::rhi::GraphicsPipelineDesc state{.color_count = 1, .debug_name = "cooked"};
    state.color_formats[0] = de::rhi::Format::RGBA8Unorm;
    const auto program = library->create_graphics({.module = "tests.solid_color", .state = state});
    REQUIRE(program.ok());
    CHECK(library->pipeline(*program).is_valid());
    const de::shader::ShaderReflection* reflection = library->reflection(*program, de::rhi::ShaderStage::Fragment);
    REQUIRE(reflection != nullptr);
    CHECK(reflection->push_constant_bytes == 16);
    const auto compute = library->create_compute({.module = "tests.fill_buffer"});
    REQUIRE(compute.ok());
    CHECK(library->pipeline(*compute).is_valid());
    const auto missing = library->create_compute({.module = "not_cooked"});
    REQUIRE(missing.ok());
    CHECK_FALSE(library->pipeline(*missing).is_valid());
    const de::shader::ShaderDefine defines[] = {{.name = "VARIANT"}};
    const auto other = library->create_compute({.module = "tests.fill_buffer", .defines = defines});
    REQUIRE(other.ok());
    CHECK_FALSE(library->pipeline(*other).is_valid()); // that permutation wasn't cooked

    // Hot reload of cooked files takes only what's stamped with this build's sources, and keeps the
    // pipeline it has otherwise, until the stamp is right again.
    auto reloading =
        de::shader::ShaderLibrary::create_cooked(*device, *vfs, jobs, {.hot_reload = true, .poll_interval_ms = 0});
    REQUIRE(reloading.ok());
    const auto watched = reloading->create_compute({.module = "tests.fill_buffer"});
    REQUIRE(watched.ok());
    const de::rhi::PipelineHandle first = reloading->pipeline(*watched);
    REQUIRE(first.is_valid());
    const de::asset::CookedPath watched_file = cooked_path(shader_asset_id("tests.fill_buffer"), AssetType::Shader);
    // The same bytes, written until the file's time shows it: file systems tick coarsely.
    const auto recook_file = [&] {
        const std::string text = read_text(*vfs, watched_file.c_str());
        const auto before = vfs->file_info(watched_file.c_str());
        REQUIRE(before.ok());
        while (vfs->file_info(watched_file.c_str())->modified_time == before->modified_time) {
            REQUIRE(vfs->write_file(watched_file.c_str(), bytes_of(text)).ok());
        }
    };
    const uint64_t built = de::shader::built_source_hash();

    recook_file();
    reloading->reload_now();
    CHECK(reloading->stats().reloads == 1);
    const de::rhi::PipelineHandle second = reloading->pipeline(*watched);
    CHECK(second != first);

    recook_file();
    REQUIRE(write_shader_stamp(*vfs, built + 1).ok()); // cooked from other sources
    reloading->reload_now();
    CHECK(reloading->stats().reloads == 1);
    CHECK(reloading->pipeline(*watched) == second);
    REQUIRE(vfs->remove_file(kShaderStampPath).ok()); // a cook running
    reloading->reload_now();
    CHECK(reloading->stats().reloads == 1);
    CHECK(reloading->pipeline(*watched) == second);
    REQUIRE(write_shader_stamp(*vfs, built).ok()); // it's settled: what was waiting comes in
    reloading->reload_now();
    CHECK(reloading->stats().reloads == 2);
    const de::rhi::PipelineHandle third = reloading->pipeline(*watched);
    CHECK(third != second);

    // The stamp going wrong while a build is on its way stops that too.
    recook_file();
    reloading->update(); // starts it: nothing else can finish it but the calls below
    CHECK(reloading->stats().compiling == 1);
    REQUIRE(write_shader_stamp(*vfs, built + 1).ok());
    reloading->reload_now();
    CHECK(reloading->stats().compiling == 0);
    CHECK(reloading->stats().reloads == 2);
    CHECK(reloading->stats().failed_reloads == 1);
    CHECK(reloading->pipeline(*watched) == third);
    REQUIRE(write_shader_stamp(*vfs, built).ok());
    reloading->reload_now();
    CHECK(reloading->stats().reloads == 3);
    CHECK(reloading->pipeline(*watched) != third);

    // Shaders cooked from other sources than this build's are refused, and refused only here:
    // compiling from source (dev builds, hot reload) doesn't look at what's cooked.
    REQUIRE(de::asset::write_shader_stamp(*vfs, de::shader::built_source_hash() + 1).ok());
    const auto stale = de::shader::ShaderLibrary::create_cooked(*device, *vfs, jobs, {.hot_reload = false});
    REQUIRE_FALSE(stale.ok());
    CHECK(std::string_view(stale.error().message).find("re-run the cooker") != std::string_view::npos);
    auto from_source = de::shader::ShaderLibrary::create(*device, *vfs, jobs, *compiler, {.hot_reload = false});
    REQUIRE(from_source.ok());
    const auto compiled_program = from_source->create_compute({.module = "tests.fill_buffer"});
    REQUIRE(compiled_program.ok());
    CHECK(from_source->pipeline(*compiled_program).is_valid());
    de::test::check_clean(*device);
}

TEST_CASE("the hash of the shader sources follows what cooked shaders are made from") {
    const Sandbox sandbox;
    write_src(sandbox, "shaders/a.slang", "float a() { return 1; }\nfloat a2() { return 2; }\n");
    write_src(sandbox, "shaders/scene/b.slang", "float b();\n");
    const Vfs vfs = cooker_vfs(sandbox);
    const auto hash = [&] {
        const de::core::Result<uint64_t> result = shader_sources_hash(vfs);
        REQUIRE(result.ok());
        return *result;
    };
    const uint64_t original = hash();
    CHECK(hash() == original);

    // Line endings don't count: a Windows and a Linux checkout hash the same.
    write_src(sandbox, "shaders/a.slang", "float a() { return 1; }\r\nfloat a2() { return 2; }\r\n");
    CHECK(hash() == original);
    // What cooked shaders don't come from doesn't count: documents, the test suite's modules, other files.
    write_src(sandbox, "shaders/readme.md", "notes");
    write_src(sandbox, "shaders/tests/solid.slang", "float t();");
    write_src(sandbox, "shaders/scene/b.slang.orig", "float b_old();");
    CHECK(hash() == original);

    // Anything else does: an edit in any file, however small.
    write_src(sandbox, "shaders/scene/b.slang", "float b(); \n");
    const uint64_t edited = hash();
    CHECK(edited != original);
    write_src(sandbox, "shaders/scene/b.slang", "float b();\n");
    CHECK(hash() == original);
    // A new file, an emptied one, a file moved, a file deleted.
    write_src(sandbox, "shaders/scene/c.slang", "");
    const uint64_t added = hash();
    CHECK(added != original);
    write_src(sandbox, "shaders/scene/c.slang", "float b();\n");
    CHECK(hash() != added);
    remove_src(sandbox, "shaders/scene/c.slang");
    CHECK(hash() == original);
    write_src(sandbox, "shaders/b.slang", "float b();\n");
    remove_src(sandbox, "shaders/scene/b.slang");
    CHECK(hash() != original);
    remove_src(sandbox, "shaders/b.slang");
    CHECK(hash() != original);

    // No sources is an answer of its own.
    remove_src(sandbox, "shaders");
    const de::core::Result<uint64_t> none = shader_sources_hash(vfs);
    REQUIRE_FALSE(none.ok());
    CHECK(none.error().code == de::core::ErrorCode::NotFound);
}

TEST_CASE("this build's shader hash is the hash of the shaders it was built from") {
    const Sandbox sandbox;
    const de::platform::PathString game_root = sandbox.game();
    const de::platform::PathString user_root = sandbox.folder("user");
    const de::platform::PathString temp_root = sandbox.folder("temp");
    const auto vfs = Vfs::create({.game_root = game_root.c_str(),
                                  .user_root = user_root.c_str(),
                                  .temp_root = temp_root.c_str(),
                                  .src_root = DE_SOURCE_DIR});
    REQUIRE(vfs.ok());
    const de::core::Result<uint64_t> sources = shader_sources_hash(*vfs);
    REQUIRE(sources.ok());
    CHECK(de::shader::built_source_hash() == *sources);
}

TEST_CASE("cooked shaders are told from other sources' by their stamp") {
    const Sandbox sandbox;
    const Vfs vfs = cooker_vfs(sandbox);
    constexpr uint64_t kHash = 0x0123456789abcdef;
    const auto code_of = [&](uint64_t hash) {
        const de::core::Result<void> checked = de::shader::check_cooked_shaders(vfs, hash);
        return checked ? de::core::ErrorCode::Unknown : checked.error().code;
    };
    const auto message_of = [&](uint64_t hash) {
        const de::core::Result<void> checked = de::shader::check_cooked_shaders(vfs, hash);
        return std::string_view(checked ? "" : checked.error().message);
    };

    // Nothing cooked: refused, saying what to do.
    CHECK(code_of(kHash) == de::core::ErrorCode::NotFound);
    CHECK(message_of(kHash).find("re-run the cooker") != std::string_view::npos);

    REQUIRE(write_shader_stamp(vfs, kHash).ok());
    const de::core::Result<uint64_t> stamp = read_shader_stamp(vfs);
    REQUIRE(stamp.ok());
    CHECK(*stamp == kHash);
    CHECK(de::shader::check_cooked_shaders(vfs, kHash).ok());
    // Near misses in either half of the hash, and a hash with leading zeros.
    CHECK(code_of(kHash + 1) == de::core::ErrorCode::CorruptData);
    CHECK(code_of(kHash ^ (uint64_t{1} << 63U)) == de::core::ErrorCode::CorruptData);
    CHECK(message_of(kHash + 1).find("re-run the cooker") != std::string_view::npos);
    REQUIRE(write_shader_stamp(vfs, 0x12).ok());
    CHECK(de::shader::check_cooked_shaders(vfs, 0x12).ok());

    // What isn't a stamp is refused, however close.
    for (const std::string_view broken :
         {"", "0123456789abcde", "0123456789abcdefa", "0123456789abcdeg", "-123456789abcdef",
          "this file is far longer than a stamp could ever be", "0x23456789abcdef", "0123456789abcdef\nmore"}) {
        sandbox.add_game_file("shader_sources.hash", broken);
        CHECK(code_of(kHash) == de::core::ErrorCode::CorruptData);
        CHECK(message_of(kHash).find("re-run the cooker") != std::string_view::npos);
    }
    // One that came through a text editor still is one.
    sandbox.add_game_file("shader_sources.hash", "0123456789abcdef\r\n");
    CHECK(de::shader::check_cooked_shaders(vfs, kHash).ok());
}

TEST_CASE("a cook stamps the shader sources it read, and --check reports cooked shaders as stale") {
    constexpr std::string_view kModule = "[shader(\"compute\")] [numthreads(1, 1, 1)] void compute_main() {}\n";
    constexpr std::string_view kEdited = "[shader(\"compute\")] [numthreads(2, 1, 1)] void compute_main() {}\n";
    constexpr std::string_view kBroken = "[shader(\"compute\")] [numthreads(1, 1, 1)] void compute_main() { nope }\n";
    constexpr std::string_view kStale = "stale";
    Sandbox sandbox;
    Vfs vfs = cooker_vfs(sandbox);
    de::core::JobSystem jobs({.worker_count = 1});
    auto compiler = de::shader::ShaderCompiler::create(vfs, {.debug_info = false, .checks = false});
    REQUIRE(compiler.ok());
    const auto cook = [&](de::shader::ShaderCompiler* with) {
        return de::cooker::cook(vfs, jobs, with, {.content = false});
    };
    const auto check = [&](bool check_shaders = true) {
        return de::cooker::cook(vfs, jobs, nullptr,
                                {.check_only = true, .content = false, .check_shaders = check_shaders});
    };
    const auto sources = [&] {
        const de::core::Result<uint64_t> result = shader_sources_hash(vfs);
        REQUIRE(result.ok());
        return *result;
    };

    // Nothing cooked yet is nothing stale: what CI sees.
    write_src(sandbox, "shaders/one.slang", kModule);
    write_src(sandbox, "shaders/library.slang", "float helper();\n");
    CHECK(check().ok());
    CHECK_FALSE(vfs.exists(kShaderStampPath));

    REQUIRE(cook(&*compiler).ok());
    const de::core::Result<uint64_t> stamp = read_shader_stamp(vfs);
    REQUIRE(stamp.ok());
    CHECK(*stamp == sources());
    CHECK(de::shader::check_cooked_shaders(vfs, sources()).ok());
    CHECK(check().ok());
    // Up to date is still stamped, and a checkout with other line endings is the same sources.
    CookReport again = cook(&*compiler);
    CHECK(again.stats.up_to_date == 1);
    CHECK(read_shader_stamp(vfs).ok());
    write_src(sandbox, "shaders/library.slang", "float helper();\r\n");
    CHECK(check().ok());

    // An edit makes them stale, even of a module with no cooked file of its own.
    write_src(sandbox, "shaders/library.slang", "float helper(); // changed\n");
    CookReport checked = check();
    CHECK_FALSE(checked.ok());
    CHECK(has_message(checked, "shaders", kStale));
    CHECK(has_message(checked, "shaders", "re-run the cooker"));
    CHECK_FALSE(de::shader::check_cooked_shaders(vfs, sources()).ok());
    // And --check still writes nothing, its stamp included.
    CHECK(*read_shader_stamp(vfs) == *stamp);
    // Unless told to leave shaders out.
    CHECK(check(false).ok());

    // Cooking without a compiler leaves the shaders as they are, and so the stamp: still stale.
    REQUIRE(cook(nullptr).ok());
    CHECK(*read_shader_stamp(vfs) == *stamp);
    CHECK_FALSE(check().ok());

    // Cooking again settles it.
    REQUIRE(cook(&*compiler).ok());
    CHECK(*read_shader_stamp(vfs) == sources());
    CHECK(*read_shader_stamp(vfs) != *stamp);
    CHECK(check().ok());
    write_src(sandbox, "shaders/one.slang", kEdited);
    CHECK_FALSE(check().ok());
    REQUIRE(cook(&*compiler).ok());
    CHECK(check().ok());

    // A cook that fails leaves a mix of old and new: no stamp, for any engine, even one built from
    // the very sources the stamp had named, and --check says so.
    const uint64_t good = sources();
    write_src(sandbox, "shaders/two.slang", kBroken);
    const CookReport broken = cook(&*compiler);
    CHECK_FALSE(broken.ok());
    CHECK(broken.stats.failed == 1);
    CHECK_FALSE(vfs.exists(kShaderStampPath));
    CHECK_FALSE(de::shader::check_cooked_shaders(vfs, good).ok());
    CHECK_FALSE(de::shader::check_cooked_shaders(vfs, sources()).ok());
    checked = check();
    CHECK(has_message(checked, "shaders", kStale));
    write_src(sandbox, "shaders/two.slang", kModule);
    REQUIRE(cook(&*compiler).ok());
    CHECK(de::shader::check_cooked_shaders(vfs, sources()).ok());
    CHECK(check().ok());

    // A test module that fails doesn't count: games never load those, and they aren't in the hash.
    write_src(sandbox, "shaders/tests/broken.slang", kBroken);
    const CookReport test_broken = de::cooker::cook(vfs, jobs, &*compiler, {.test_shaders = true, .content = false});
    CHECK(test_broken.stats.failed == 1);
    CHECK(de::shader::check_cooked_shaders(vfs, sources()).ok());
    remove_src(sandbox, "shaders/tests");
    REQUIRE(cook(&*compiler).ok());

    // Files cooked by a cooker from before stamps, or a stamp lost.
    REQUIRE(vfs.remove_file(kShaderStampPath).ok());
    checked = check();
    CHECK(has_message(checked, "shaders", kStale));
    // A stamp that isn't one.
    sandbox.add_game_file("shader_sources.hash", "garbled");
    checked = check();
    CHECK(has_message(checked, "shaders", "re-run the cooker"));
    // Cooking heals both.
    REQUIRE(cook(&*compiler).ok());
    CHECK(check().ok());

    // A first cook killed halfway, or a state file lost: what tells is the cooked files.
    REQUIRE(vfs.remove_file(kShaderStampPath).ok());
    REQUIRE(vfs.remove_file(de::cooker::CookState::kPath).ok());
    checked = check();
    CHECK(has_message(checked, "shaders", kStale));
    REQUIRE(cook(&*compiler).ok());
    CHECK(check().ok());

    // A shader saved during the cook: what's cooked may be from either side, so no stamp, and a
    // failed cook. The next one cooks every module, trusting no record of this one.
    de::cooker::CookOptions saving{.content = false};
    saving.before_shader_stamp = [](void* context) {
        write_src(*static_cast<const Sandbox*>(context), "shaders/library.slang", "float helper(); // saved\n");
    };
    saving.before_shader_stamp_context = &sandbox;
    const CookReport saved = de::cooker::cook(vfs, jobs, &*compiler, saving);
    CHECK_FALSE(saved.ok());
    CHECK(has_message(saved, "shaders", "changed during the cook"));
    CHECK_FALSE(vfs.exists(kShaderStampPath));
    CHECK_FALSE(de::shader::check_cooked_shaders(vfs, sources()).ok());
    again = cook(&*compiler);
    CHECK(again.ok());
    CHECK(again.stats.cooked == 2);
    CHECK(again.stats.up_to_date == 0);
    CHECK(*read_shader_stamp(vfs) == sources());

    // The old stamp can't be removed (here, a folder has its name): it would outlive a partial
    // cook, so the shaders aren't touched, and the cook failed.
    const de::asset::CookedPath one = cooked_path(shader_asset_id("one"), AssetType::Shader);
    const std::string before_stuck = read_text(vfs, one.c_str());
    REQUIRE(vfs.remove_file(kShaderStampPath).ok());
    sandbox.add_game_file("shader_sources.hash/in_the_way", "x");
    write_src(sandbox, "shaders/one.slang", kModule);
    const CookReport stuck = cook(&*compiler);
    CHECK_FALSE(stuck.ok());
    CHECK(has_message(stuck, "", "could not be removed"));
    CHECK(stuck.stats.cooked == 0);
    CHECK(stuck.stats.removed == 0);
    CHECK(read_text(vfs, one.c_str()) == before_stuck);
    de::platform::PathString in_the_way = sandbox.game();
    REQUIRE(in_the_way.append("/shader_sources.hash"));
    REQUIRE(de::platform::delete_tree(in_the_way.c_str()).ok());
    REQUIRE(cook(&*compiler).ok());
    CHECK(read_text(vfs, one.c_str()) != before_stuck);
    CHECK(check().ok());

    // A module that can't be cooked for its name is in the hash but not in game:/: no stamp. Other
    // files in the tree aren't modules, and say nothing.
    constexpr std::string_view kLongName =
        "shaders/a_name_longer_than_any_module_can_have_for_the_shader_library_to_hold.slang";
    write_src(sandbox, kLongName, kModule);
    const CookReport too_long = cook(&*compiler);
    CHECK(has_message(too_long, kLongName, "longer than"));
    CHECK_FALSE(vfs.exists(kShaderStampPath));
    remove_src(sandbox, kLongName);
    write_src(sandbox, "shaders/notes.md", "notes");
    const CookReport notes = cook(&*compiler);
    CHECK(notes.ok());
    CHECK(check().ok());
}

TEST_CASE("normals pack to two snorm16 and back") {
    const float inputs[][3] = {{0, 0, 1}, {0, 0, -1}, {1, 0, 0}, {0, -1, 0}, {0.5f, -0.5f, -0.7f}, {0.6f, 0.0f, 0.8f}};
    for (const auto& input : inputs) {
        float decoded[3];
        de::cooker::decode_octahedral(de::cooker::encode_octahedral(input), decoded);
        const float length = std::sqrt(input[0] * input[0] + input[1] * input[1] + input[2] * input[2]);
        for (int axis = 0; axis < 3; ++axis) {
            CHECK(decoded[axis] == doctest::Approx(input[axis] / length).epsilon(0.001));
        }
    }
}

TEST_CASE("cells place models by path, parents first, and cook again when a model's ID changes") {
    const Sandbox sandbox;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    quad(vertices, indices);
    write_src(sandbox, "content/props/crate.gltf", gltf(vertices, indices, 0.0f, R"([{"name": "paint"}])", "[]", "[]"));
    write_src(sandbox, "content/props/lamp.gltf", gltf(vertices, indices, 1.0f, R"([{"name": "paint"}])", "[]", "[]"));
    write_src(sandbox, "content/props/crate.png", png(8, 8, color_texels(8, 1)));
    // The child comes before its parent in the source: the cooked file puts the parent first.
    write_src(sandbox, "content/world/cell_1_2.cell", R"({
  "coord": [1, 2],
  "entities": [
    {"name": "lamp", "parent": "post", "mesh": "../props/lamp.gltf", "position": [0, 2, 0], "static": false},
    {"name": "post", "mesh": "../props/crate.gltf", "position": [130, 0, 260], "rotation_deg": [0, 90, 0], "scale": 2},
    {"mesh": "../props/crate.gltf", "position": [200, 0.5, 300.25], "rotation": [0, 0, 0, 2], "scale": [1, 2, 3]},
    {"name": "flame", "parent": "lamp", "position": [0, 0.5, 0], "rotation": [1e20, 0, 0, 1]},
  ],
})");
    write_src(sandbox, "content/world/broken.cell", R"({
  "coord": [0, 0.5],
  "entities": [
    {"name": "a", "parent": "b"}, {"name": "b", "parent": "a"},
    {"name": "c", "parent": "nobody", "mesh": "../props/crate.png"},
    {"name": "c", "scale": [1, 0, 1], "colour": 1},
    {"name": "far", "position": [-5, 0, 0]},
    {"parent": "ghost", "position": [-3, 0, 0]},
    {"name": "into_loop", "parent": "a"},
    {"name": "huge", "scale": 1e39},
    {"name": "cart", "static": false},
    {"name": "wheel", "parent": "cart", "static": true},
    {"name": "mist_a", "fog_volume": 1},
    {"name": "mist_b", "fog_volume": {"shape": "cube", "density_per_m": 0.2}},
    {"name": "mist_c", "fog_volume": {"edge_m": 2}},
    {"name": "mist_d", "fog_volume": {"density_per_m": 0.2, "edge_m": -1, "tint": [1, 1], "paint": 1}},
    {"name": "mist_e", "fog_volume": {"density_per_m": 5}},
    {"name": "mist_f", "fog_volume": {"density_per_m": 0.2, "edge_m": "wide"}},
    {"name": "mist_g", "fog_volume": {"density_per_m": 0.2, "noise_amount": 2, "noise_size_m": 0, "drift_mps": [1, 2], "height_fade_m": -1}},
    {"name": "mist_h", "fog_volume": {"density_per_m": 0.2, "noise_size_m": 1e-60}},
  ],
})");
    write_src(sandbox, "content/world/cell_5_5.cell", R"({
  "coord": [5, 5],
  "entities": [
    {"name": "mist", "position": [700, 0, 700], "scale": [8, 3, 8],
     "fog_volume": {"shape": "ellipsoid", "density_per_m": 0.2, "edge_m": 2, "tint": [0.5, 0.6, 0.7],
                    "noise_amount": 0.75, "noise_size_m": 5, "drift_mps": [0.5, 0.25, 1.5], "height_fade_m": 3}},
    {"name": "fog wall", "position": [710, 0, 700], "fog_volume": {"density_per_m": 0.1}},
    {"name": "plain", "position": [720, 0, 700]},
  ],
})");
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    const char* broken = "content/world/broken.cell";
    CHECK(has_message(report, broken, "coord should be [x, z]"));
    CHECK(has_message(report, broken, "its parents loop back to it"));
    CHECK(has_message(report, broken, "parent 'nobody' isn't another entity"));
    CHECK(has_message(report, broken, "entity 5: parent 'ghost' isn't another entity")); // unnamed: by index
    CHECK_FALSE(has_message(report, broken, "entity 5: lies outside"));                  // its position is local
    CHECK_FALSE(has_message(report, broken, "into_loop: its parents loop")); // leads into a loop, isn't on it
    CHECK(has_message(report, broken, "huge: scale is too large for a float"));
    CHECK(has_message(report, broken, "wheel: can't be static: its parent 'cart' moves"));
    CHECK(has_message(report, broken, "should name a model source"));
    CHECK(has_message(report, broken, "another entity has its name"));
    CHECK(has_message(report, broken, "scale has a zero"));
    CHECK(has_message(report, broken, "unknown cell entity setting 'colour'"));
    CHECK(has_message(report, broken, "mist_a: fog_volume should be an object"));
    CHECK(has_message(report, broken, "mist_b: fog_volume shape should be box or ellipsoid"));
    CHECK(has_message(report, broken, "mist_c: fog_volume needs a density_per_m above 0"));
    CHECK(has_message(report, broken, "mist_d: fog_volume edge_m should be 0 or more"));
    CHECK(has_message(report, broken, "mist_d: fog_volume tint should be 3 numbers"));
    CHECK(has_message(report, broken, "unknown fog volume setting 'paint'"));
    CHECK(has_message(report, broken, "mist_e: fog_volume needs a density_per_m above 0, up to 4"));
    CHECK(has_message(report, broken, "'edge_m' should be a number"));
    CHECK(has_message(report, broken, "mist_g: fog_volume noise_size_m should be above 0"));
    CHECK(has_message(report, broken, "mist_h: fog_volume noise_size_m should be above 0"));
    CHECK(has_message(report, broken, "mist_g: fog_volume noise_amount should be from 0 to 1"));
    CHECK(has_message(report, broken, "mist_g: fog_volume drift_mps should be 3 numbers"));
    CHECK(has_message(report, broken, "mist_g: fog_volume height_fade_m should be 0 or more"));
    CHECK_FALSE(has_message(report, "content/world/cell_5_5.cell", ""));
    CHECK(report.stats.failed == 1);
    CHECK_FALSE(has_message(report, "content/world/cell_1_2.cell", ""));

    const AssetId crate = asset_id(guid_of(vfs, "src:/meta/props/crate.gltf.json"));
    const AssetId lamp = asset_id(guid_of(vfs, "src:/meta/props/lamp.gltf.json"));
    const AssetId cell_id = asset_id(guid_of(vfs, "src:/meta/world/cell_1_2.cell.json"));
    {
        Cooked mist_file(vfs, asset_id(guid_of(vfs, "src:/meta/world/cell_5_5.cell.json")), AssetType::Cell);
        const auto& mist_cell = mist_file.load<CellAsset>();
        const auto volumes = mist_cell.fog_volumes.get(mist_file.data());
        REQUIRE(volumes.size() == 2);
        const auto placed = mist_cell.entities.get(mist_file.data());
        REQUIRE(placed.size() == 3);
        CHECK(placed[0].fog_volume == 0);
        CHECK(placed[1].fog_volume == 1);
        CHECK(placed[2].fog_volume == CellEntity::kNone);
        CHECK(volumes[0].shape == de::asset::CellFogShape::Ellipsoid);
        CHECK(volumes[0].density_per_m == 0.2f);
        CHECK(volumes[0].edge_m == 2.0f);
        CHECK(volumes[0].tint[2] == 0.7f);
        CHECK(volumes[0].noise_amount == 0.75f);
        CHECK(volumes[0].noise_size_m == 5.0f);
        CHECK(volumes[0].drift_mps[1] == 0.25f);
        CHECK(volumes[0].height_fade_m == 3.0f);
        CHECK(volumes[1].shape == de::asset::CellFogShape::Box);
        CHECK(volumes[1].edge_m == 1.0f); // by default
        CHECK(volumes[1].tint[0] == 1.0f);
        CHECK(volumes[1].noise_amount == 0.0f);
        CHECK(volumes[1].noise_size_m > 0.0f);
        CHECK(volumes[1].drift_mps[0] == 0.0f);
        CHECK(volumes[1].height_fade_m == 0.0f);
    }
    {
        Cooked file(vfs, cell_id, AssetType::Cell);
        const auto& cell = file.load<CellAsset>();
        CHECK(cell.coord_x == 1);
        CHECK(cell.coord_z == 2);
        const auto dependencies = cell.header.dependencies.get(file.data());
        REQUIRE(dependencies.size() == 2); // the crate once, though placed twice
        CHECK(dependencies[0] == AssetRef{.id = lamp, .type = AssetType::Mesh});
        CHECK(dependencies[1] == AssetRef{.id = crate, .type = AssetType::Mesh});
        const auto entities = cell.entities.get(file.data());
        REQUIRE(entities.size() == 4);
        CHECK((cell.name(file.data(), entities[0]) == "post"));
        CHECK((cell.name(file.data(), entities[1]) == ""));
        CHECK((cell.name(file.data(), entities[2]) == "lamp"));
        CHECK(entities[0].parent == CellEntity::kNone);
        CHECK(entities[2].parent == 0);
        CHECK(entities[2].mesh == 0);
        CHECK(entities[0].mesh == 1);
        CHECK(entities[1].mesh == 1);
        CHECK(entities[0].position[0] == 130.0);
        CHECK(entities[1].position[2] == 300.25);
        // 90 degrees of yaw; scales uniform and per axis; a quaternion is normalized.
        CHECK(entities[0].rotation[1] == doctest::Approx(std::sqrt(0.5)));
        CHECK(entities[0].rotation[3] == doctest::Approx(std::sqrt(0.5)));
        CHECK(entities[0].scale[2] == 2.0f);
        CHECK(entities[1].scale[2] == 3.0f);
        CHECK(entities[1].rotation[3] == 1.0f);
        CHECK((entities[0].flags & CellEntityFlags::Static) == CellEntityFlags::Static);
        CHECK((entities[2].flags & CellEntityFlags::Static) == CellEntityFlags::None);
        // The flame moves with the lamp (not static by default), its huge quaternion normalized.
        CHECK((cell.name(file.data(), entities[3]) == "flame"));
        CHECK(entities[3].parent == 2);
        CHECK((entities[3].flags & CellEntityFlags::Static) == CellEntityFlags::None);
        CHECK(entities[3].rotation[0] == doctest::Approx(1.0f));
        CHECK(entities[3].rotation[3] == doctest::Approx(0.0f));
    }
    // Cooking without content (shaders only) keeps what was cooked from it.
    const CookReport shaders_only = run_cook(vfs, {.content = false});
    CHECK(shaders_only.stats.removed == 0);
    CHECK(vfs.exists(cooked_path(cell_id, AssetType::Cell).c_str()));
    CHECK(run_cook(vfs).stats.up_to_date == 5); // and the state still knows it
    // A root outside its cell is only a warning.
    write_src(sandbox, broken, R"({"coord": [0, 0], "entities": [{"name": "far", "position": [-5, 0, 0]}]})");
    const CookReport fixed = run_cook(vfs);
    CHECK(has_message(fixed, broken, "far: lies outside the cell [0, 0]"));
    CHECK(fixed.stats.failed == 0);

    // A model cooked again (new bytes, same ID) leaves the cell alone; a model with a new ID cooks it
    // again, pointing at the new ID.
    write_src(sandbox, "content/props/lamp.gltf", gltf(vertices, indices, 3.0f, R"([{"name": "paint"}])", "[]", "[]"));
    const CookReport touched = run_cook(vfs);
    CHECK(touched.stats.cooked == 1);
    remove_src(sandbox, "meta/props/lamp.gltf.json");
    const CookReport reminted = run_cook(vfs);
    const AssetId new_lamp = asset_id(guid_of(vfs, "src:/meta/props/lamp.gltf.json"));
    REQUIRE(new_lamp != lamp);
    CHECK(reminted.stats.cooked == 2); // the lamp and the cell
    Cooked file(vfs, cell_id, AssetType::Cell);
    const auto& cell = file.load<CellAsset>();
    CHECK(cell.header.dependencies.get(file.data())[0].id == new_lamp);
}

TEST_CASE("a glTF buffer shorter than the glTF says is reported, not read past") {
    const Sandbox sandbox;
    // What a Git LFS pointer looks like where the buffer should be.
    write_src(sandbox, "content/props/rock.bin", "version https://git-lfs.github.com/spec/v1\n");
    write_src(sandbox, "content/props/rock.gltf",
              R"({"asset": {"version": "2.0"}, "scene": 0, "scenes": [{"nodes": [0]}],
  "nodes": [{"mesh": 0}], "meshes": [{"primitives": [{"attributes": {"POSITION": 0}}]}],
  "buffers": [{"byteLength": 4096, "uri": "rock.bin"}],
  "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 4096}],
  "accessors": [{"bufferView": 0, "componentType": 5126, "count": 300, "type": "VEC3",
                 "min": [0, 0, 0], "max": [1, 1, 1]}]})");
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    CHECK(has_message(report, "content/props/rock.gltf", "shorter than the 4096 the glTF says"));
    CHECK(report.stats.failed == 1);
}

namespace {

constexpr const char* kEvening = R"({
  "sun": {"elevation_deg": 30, "azimuth_deg": 90, "color": [1, 0.5, 0.25], "intensity": 4},
  "sky": {"color": [0.3, 0.4, 0.6]},
  "ground": {"color": [0.1, 0.1, 0.1], "intensity": 0.5},
  "shadow_color": [0.05, 0.06, 0.09],
  "exposure_ev": -1,
  "shadows": {"resolution": 1024, "distance_m": 100, "split_lambda": 0.5, "blend_band": 0.1,
              "caster_reach_m": 150, "penumbra_per_m": 0.05, "max_penumbra_texels": 4},
  "fog": {"density_per_m": 0.0001, "height_density_per_m": 0.01, "height_falloff_per_m": 0.1, "base_height_m": -20,
          "extinction_tint": [0.8, 1, 1.25], "sun_forward": {"color": [1, 0.5, 0.25], "intensity": 2},
          "sun_back": {"color": [0.1, 0.2, 0.3]}, "sun_height_attenuation": 0.5,
          "ambient": {"color": [0.2, 0.3, 0.4], "intensity": 0.5}, "ambient_height_attenuation": 2,
          "start_m": 1, "sun_start_m": 4, "end_m": 5000, "volume_distance_m": 90}
})";

} // namespace

TEST_CASE("worlds list their cells by coord, with an environment, and cook again when a cell moves") {
    const Sandbox sandbox;
    write_src(sandbox, "content/world/a/cell_0_0.cell", R"({"coord": [0, 0], "entities": []})");
    write_src(sandbox, "content/world/a/cell_1_0.cell", R"({"coord": [1, 0], "entities": []})");
    write_src(sandbox, "content/world/a/cell_0_1.cell", R"({"coord": [0, 1], "entities": []})");
    write_src(sandbox, "content/world/a/evening.environment", kEvening);
    // Listed out of order: the cooked list is by coord, z then x.
    write_src(sandbox, "content/world/a/a.world", R"({
  "environment": "evening.environment",
  "start": {"position": [10, 2, -3.5], "yaw_deg": 90, "pitch_deg": -10},
  "cells": ["cell_0_1.cell", "cell_1_0.cell", "cell_0_0.cell"],
})");
    write_src(sandbox, "content/world/a/broken.world", R"({
  "environment": "cell_0_0.cell",
  "start": {"position": [0, 0], "roll_deg": 3},
  "cells": ["cell_0_0.cell", "nowhere.cell", "same_place.cell"],
})");
    write_src(sandbox, "content/world/a/same_place.cell", R"({"coord": [0, 0], "entities": []})");
    write_src(sandbox, "content/world/a/broken.environment", R"({
  "sun": {"elevation_deg": 120, "azimuth_deg": 0, "color": [1, 1, 1]},
  "sky": {"color": [0.3, -0.4, 0.6]},
  "shadow_color": [0, 0, 0],
  "exposure": 0,
  "shadows": {"resolution": 1000.5, "distance_m": 100, "split_lambda": 2, "blend_band": 0.1,
              "caster_reach_m": 150, "penumbra_per_m": 0.05},
  "fog": {"density_per_m": 2, "height_density_per_m": 0.01, "height_falloff_per_m": 0.1, "base_height_m": 0,
          "extinction_tint": [0, 1, 1], "sun_forward": {"color": [1, 1, 1]}, "sun_back": {"color": [0, 0, 0]},
          "sun_height_attenuation": 1, "ambient": {"color": [0.1, 0.1, 0.1]}, "ambient_height_attenuation": 1,
          "start_m": 100, "sun_start_m": 0, "end_m": 50, "volume_distance_m": 5000, "colour": 1}
})");
    // A typo in end_m: its own error, not also "end_m should be past start_m" against a default.
    std::string typo = kEvening;
    const std::string_view end = R"("end_m": 5000)";
    const std::string_view start = R"("start_m": 1,)";
    typo.replace(typo.find(end), end.size(), R"("end_m": "far")");
    typo.replace(typo.find(start), start.size(), R"("start_m": 30000,)");
    write_src(sandbox, "content/world/a/typo.environment", typo);
    Vfs vfs = cooker_vfs(sandbox);
    const CookReport report = run_cook(vfs);
    CHECK(has_message(report, "content/world/a/typo.environment", "end_m should be a number"));
    CHECK_FALSE(has_message(report, "content/world/a/typo.environment", "past its start_m"));
    const char* broken_world = "content/world/a/broken.world";
    CHECK(has_message(report, broken_world, "'cell_0_0.cell' should name an environment source"));
    CHECK(has_message(report, broken_world, "start should have a position"));
    CHECK(has_message(report, broken_world, "'nowhere.cell' should name a cell source"));
    CHECK(has_message(report, broken_world, "two cells at [0, 0]"));
    const char* broken_environment = "content/world/a/broken.environment";
    CHECK(has_message(report, broken_environment, "elevation_deg should be a number from -90 to 90"));
    CHECK(has_message(report, broken_environment, "sky should have a color"));
    CHECK(has_message(report, broken_environment, "ground should have a color"));
    CHECK(has_message(report, broken_environment, "unknown environment setting 'exposure'"));
    CHECK(has_message(report, broken_environment, "exposure_ev should be a number"));
    CHECK(has_message(report, broken_environment, "resolution should be a whole number"));
    CHECK(has_message(report, broken_environment, "split_lambda should be a number from 0 to 1"));
    CHECK(has_message(report, broken_environment, "max_penumbra_texels should be a number from 1 to 64"));
    CHECK(has_message(report, broken_environment, "density_per_m should be a number from 0 to 1"));
    CHECK(has_message(report, broken_environment, "extinction_tint should be [r, g, b]"));
    CHECK(has_message(report, broken_environment, "end_m should be past its start_m"));
    CHECK(has_message(report, broken_environment, "unknown fog setting 'colour'"));
    CHECK(has_message(report, broken_environment, "volume_distance_m should be a number from 0 to 2000"));
    CHECK(report.stats.failed == 3);
    CHECK_FALSE(has_message(report, "content/world/a/a.world", ""));
    CHECK_FALSE(has_message(report, "content/world/a/evening.environment", ""));

    const AssetId world_id = asset_id(guid_of(vfs, "src:/meta/world/a/a.world.json"));
    const AssetId environment_id = asset_id(guid_of(vfs, "src:/meta/world/a/evening.environment.json"));
    const AssetId cell_00 = asset_id(guid_of(vfs, "src:/meta/world/a/cell_0_0.cell.json"));
    const AssetId cell_10 = asset_id(guid_of(vfs, "src:/meta/world/a/cell_1_0.cell.json"));
    const AssetId cell_01 = asset_id(guid_of(vfs, "src:/meta/world/a/cell_0_1.cell.json"));
    {
        Cooked file(vfs, world_id, AssetType::World);
        const auto& world = file.load<WorldAsset>();
        CHECK(world.start_position[0] == 10.0);
        CHECK(world.start_position[2] == -3.5);
        CHECK(world.start_yaw_rad == doctest::Approx(1.5707963));
        CHECK(world.start_pitch_rad == doctest::Approx(-0.1745329));
        const auto dependencies = world.header.dependencies.get(file.data());
        REQUIRE(dependencies.size() == 1);
        CHECK(dependencies[world.environment] == AssetRef{.id = environment_id, .type = AssetType::Environment});
        const auto cells = world.cells.get(file.data());
        REQUIRE(cells.size() == 3);
        CHECK(cells[0].cell == cell_00);
        CHECK(cells[1].cell == cell_10);
        CHECK(cells[1].coord_x == 1);
        CHECK(cells[2].cell == cell_01);
        CHECK(cells[2].coord_z == 1);
    }
    {
        Cooked file(vfs, environment_id, AssetType::Environment);
        const auto& environment = file.load<EnvironmentAsset>();
        // 30 degrees up, due east (+X).
        CHECK(environment.sun_direction[0] == doctest::Approx(0.8660254));
        CHECK(environment.sun_direction[1] == doctest::Approx(0.5));
        CHECK(environment.sun_direction[2] == doctest::Approx(0.0).epsilon(1e-6));
        CHECK(environment.sun_color[1] == doctest::Approx(2.0f)); // times its intensity
        CHECK(environment.sky_color[2] == doctest::Approx(0.6f)); // intensity 1 by default
        CHECK(environment.ground_color[0] == doctest::Approx(0.05f));
        CHECK(environment.exposure_ev == -1.0f);
        CHECK(environment.shadow_resolution == 1024);
        CHECK(environment.shadow_split_lambda == 0.5f);
        CHECK(environment.shadow_max_penumbra_texels == 4.0f);
        CHECK(environment.fog_density_per_m == doctest::Approx(0.0001f));
        CHECK(environment.fog_height_density_per_m == doctest::Approx(0.01f));
        CHECK(environment.fog_base_height_m == -20.0f);
        CHECK(environment.fog_extinction_tint[2] == doctest::Approx(1.25f));
        CHECK(environment.fog_sun_forward[1] == doctest::Approx(1.0f)); // times its intensity
        CHECK(environment.fog_sun_back[2] == doctest::Approx(0.3f));    // intensity 1 by default
        CHECK(environment.fog_ambient[0] == doctest::Approx(0.1f));
        CHECK(environment.fog_ambient_height_attenuation == 2.0f);
        CHECK(environment.fog_sun_start_m == 4.0f);
        CHECK(environment.fog_end_m == 5000.0f);
        CHECK(environment.fog_volume_distance_m == 90.0f);
    }
    // A cell moved to another coord cooks the world again; new light only cooks the environment.
    CHECK(run_cook(vfs).stats.cooked == 0);
    write_src(sandbox, "content/world/a/evening.environment",
              std::string(kEvening).replace(std::string(kEvening).find("-1"), 2, "-2"));
    CHECK(run_cook(vfs).stats.cooked == 1);
    write_src(sandbox, "content/world/a/cell_1_0.cell", R"({"coord": [2, 0], "entities": []})");
    CHECK(run_cook(vfs).stats.cooked == 2); // the cell and the world
    Cooked file(vfs, world_id, AssetType::World);
    CHECK(file.load<WorldAsset>().cells.get(file.data())[1].coord_x == 2);
}

TEST_CASE("broken worlds and environments are refused when loaded") {
    struct alignas(16) Aligned {
        std::byte bytes[512];
    };
    const auto check = [](const de::core::Array<std::byte>& cooked, auto loader) {
        Aligned aligned{};
        REQUIRE(cooked.size() <= sizeof(aligned.bytes));
        std::memcpy(aligned.bytes, cooked.data(), cooked.size());
        return loader(std::span(aligned.bytes, cooked.size())).ok();
    };
    const auto load_world = [](std::span<const std::byte> bytes) { return load_in_place<WorldAsset>(bytes); };
    const auto load_environment = [](std::span<const std::byte> bytes) {
        return load_in_place<EnvironmentAsset>(bytes);
    };
    const AssetId id = AssetId::from_value(0x77);
    de::cooker::WorldBuild world;
    world.environment = {.id = AssetId::from_value(0x78), .type = AssetType::Environment};
    world.cells.push({.cell = AssetId::from_value(1), .coord_x = 0, .coord_z = 0});
    world.cells.push({.cell = AssetId::from_value(2), .coord_x = 1, .coord_z = 0});
    CHECK(check(de::cooker::cook_world(world, id), load_world));
    world.cells[1].coord_x = 0; // two cells at one coord
    CHECK_FALSE(check(de::cooker::cook_world(world, id), load_world));
    world.cells[1].coord_x = -1; // out of order along x
    CHECK_FALSE(check(de::cooker::cook_world(world, id), load_world));
    world.cells[1].coord_x = 5; // along z: a later x, an earlier row
    world.cells[1].coord_z = -1;
    CHECK_FALSE(check(de::cooker::cook_world(world, id), load_world));
    world.cells[1].coord_x = 1;
    world.cells[1].coord_z = 0;
    world.environment.type = AssetType::Cell; // the environment isn't one
    CHECK_FALSE(check(de::cooker::cook_world(world, id), load_world));
    world.environment.type = AssetType::Environment;
    world.start_position[1] = std::nan("");
    CHECK_FALSE(check(de::cooker::cook_world(world, id), load_world));

    EnvironmentAsset environment;
    environment.shadow_distance_m = 100.0f;
    CHECK(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.sun_direction[1] = 2.0f; // not unit length
    CHECK_FALSE(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.sun_direction[1] = 1.0f;
    environment.sky_color[2] = -0.1f;
    CHECK_FALSE(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.sky_color[2] = 0.0f;
    environment.shadow_resolution = 16;
    CHECK_FALSE(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.shadow_resolution = 2048;
    environment.shadow_max_penumbra_texels = 0.5f;
    CHECK_FALSE(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.shadow_max_penumbra_texels = 1e9f; // an unbounded search
    CHECK_FALSE(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.shadow_max_penumbra_texels = 4.0f;
    environment.shadow_distance_m = 0.0f; // no cascades
    CHECK_FALSE(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.shadow_distance_m = 100.0f;
    CHECK(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.fog_end_m = environment.fog_start_m; // nothing between them
    CHECK_FALSE(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.fog_end_m = 20000.0f;
    environment.fog_extinction_tint[1] = 0.0f;
    CHECK_FALSE(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.fog_extinction_tint[1] = 1.0f;
    environment.fog_height_density_per_m = 5.0f;
    CHECK_FALSE(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.fog_height_density_per_m = 0.0f;
    environment.fog_volume_distance_m = 3000.0f; // a grid that far is out of range
    CHECK_FALSE(check(de::cooker::cook_environment(environment, id), load_environment));
    environment.fog_volume_distance_m = 128.0f;
    CHECK(check(de::cooker::cook_environment(environment, id), load_environment));
}
