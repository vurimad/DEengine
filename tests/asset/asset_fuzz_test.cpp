// Cooked files come off disk, so load_in_place() is the line between bytes and engine code. These
// tests corrupt real cooked files in many ways and check, with code independent of the validators,
// that every file load_in_place() accepts is safe to use: every array, mip, part and entry the
// asset hands out lies inside the file, and every number a consumer sizes things by is sane.

#include "asset/cell_format.h"
#include "asset/format.h"
#include "asset/material_format.h"
#include "asset/mesh_format.h"
#include "asset/shader_format.h"
#include "asset/texture_format.h"
#include "cooker/blob_writer.h"
#include "cooker/cell_cooker.h"
#include "cooker/material_cooker.h"
#include "cooker/mesh_cooker.h"
#include "cooker/texture_cooker.h"

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <string_view>
#include <vector>

using namespace de::asset;

namespace {

// Deterministic: a failure reproduces.
class Random {
public:
    explicit Random(uint64_t seed) : state_(seed) {}
    uint64_t next() {
        uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30U)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27U)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31U);
    }
    uint32_t below(uint32_t bound) { return static_cast<uint32_t>(next() % bound); }

private:
    uint64_t state_;
};

// A file in 16-byte aligned memory.
class Buffer {
public:
    explicit Buffer(std::span<const std::byte> bytes) : size_(bytes.size()) {
        blocks_.resize((bytes.size() + 15) / 16);
        std::memcpy(blocks_.data(), bytes.data(), bytes.size());
    }
    std::span<std::byte> bytes() { return {reinterpret_cast<std::byte*>(blocks_.data()), size_}; }

private:
    struct alignas(16) Block {
        std::byte bytes[16];
    };
    std::vector<Block> blocks_;
    size_t size_;
};

// Independent of the validators: [offset, offset + size) inside the file.
bool inside(std::span<const std::byte> file, uint64_t offset, uint64_t size) {
    return offset <= file.size() && size <= file.size() - offset;
}

template <typename T>
bool inside(std::span<const std::byte> file, OffsetSpan<T> span) {
    return span.count == 0 || inside(file, span.offset, uint64_t{span.count} * sizeof(T));
}

// What a consumer of each type relies on, checked after a successful load.
bool safe(std::span<const std::byte> file, const MeshAsset& mesh) {
    if (!inside(file, mesh.vertices) || !inside(file, mesh.indices) || !inside(file, mesh.lods) ||
        !inside(file, mesh.parts) || !inside(file, mesh.materials) || mesh.lods.count == 0) {
        return false;
    }
    for (const MeshLod& lod : mesh.lods.get(file.data())) {
        if (uint64_t{lod.first_part} + lod.part_count > mesh.parts.count) {
            return false;
        }
    }
    for (const MeshPart& part : mesh.parts.get(file.data())) {
        if (uint64_t{part.first_index} + part.index_count > mesh.indices.count ||
            part.material_slot >= mesh.materials.count) {
            return false;
        }
    }
    for (const uint32_t index : mesh.indices.get(file.data())) {
        if (index >= mesh.vertices.count) {
            return false; // a GPU read out of the vertex buffer
        }
    }
    return true;
}

bool safe(std::span<const std::byte> file, const TextureAsset& texture) {
    if (!inside(file, texture.mips) || texture.mips.count == 0 || texture.width == 0 || texture.height == 0 ||
        texture.width > 32768 || texture.height > 32768) {
        return false;
    }
    for (uint32_t i = 0; i < texture.mips.count; ++i) {
        const TextureMip& mip = texture.mips.get(file.data())[i];
        if (!inside(file, mip.offset, mip.size) || mip.width == 0 || mip.height == 0 ||
            uint64_t{mip.row_bytes} * mip.rows != mip.size || mip.size == 0) {
            return false;
        }
        const std::span<const std::byte> data = texture.mip_data(file.data(), i);
        if (data.data() < file.data() || data.data() + data.size() > file.data() + file.size()) {
            return false;
        }
    }
    return true;
}

bool safe(std::span<const std::byte> file, const MaterialAsset& material) {
    if (!inside(file, material.params) || !inside(file, material.textures)) {
        return false;
    }
    static_cast<void>(material.find_param(file.data(), material::kRoughness));
    static_cast<void>(material.find_texture(file.data(), material::kNormalTexture));
    return true;
}

// What spawning relies on: parents already made, meshes among the dependencies, names in the file,
// finite transforms.
bool safe(std::span<const std::byte> file, const CellAsset& cell) {
    if (!inside(file, cell.fog_volumes) || !inside(file, cell.entities) || !inside(file, cell.names) ||
        !inside(file, cell.header.dependencies)) {
        return false;
    }
    const auto entities = cell.entities.get(file.data());
    for (size_t i = 0; i < entities.size(); ++i) {
        const CellEntity& entity = entities[i];
        if ((entity.parent != CellEntity::kNone && entity.parent >= i) ||
            (entity.mesh != CellEntity::kNone &&
             (entity.mesh >= cell.header.dependencies.count ||
              cell.header.dependencies.get(file.data())[entity.mesh].type != AssetType::Mesh)) ||
            uint64_t{entity.name_offset} + entity.name_length > cell.names.count ||
            (entity.fog_volume != CellEntity::kNone && entity.fog_volume >= cell.fog_volumes.count) ||
            !std::isfinite(entity.position[0]) || !std::isfinite(entity.scale[1]) ||
            !std::isfinite(entity.rotation[3])) {
            return false;
        }
        static_cast<void>(cell.name(file.data(), entity));
    }
    return true;
}

bool safe(std::span<const std::byte> file, const ShaderAsset& shader) {
    if (!inside(file, shader.permutations) || std::memchr(shader.module, 0, sizeof(shader.module)) == nullptr) {
        return false;
    }
    for (const ShaderPermutation& permutation : shader.permutations.get(file.data())) {
        if (!inside(file, permutation.entries)) {
            return false;
        }
        for (const ShaderEntryCode& entry : permutation.entries.get(file.data())) {
            if (!inside(file, entry.code) || !inside(file, entry.push_constants) || entry.code.count == 0 ||
                entry.code.get(file.data())[0] != kSpirvMagic ||
                std::memchr(entry.entry, 0, sizeof(entry.entry)) == nullptr) {
                return false; // drivers may crash on what isn't SPIR-V
            }
        }
    }
    return true;
}

// Corrupts a copy of file many ways; every copy that still loads must be safe. The asset struct
// (where every offset and count lives) takes most of the hits.
template <typename T>
void fuzz(std::span<const std::byte> original, uint64_t seed, int rounds) {
    Random random(seed);
    int accepted = 0;
    for (int round = 0; round < rounds; ++round) {
        Buffer file(original);
        const std::span<std::byte> bytes = file.bytes();
        const uint32_t edits = 1 + random.below(4);
        for (uint32_t e = 0; e < edits; ++e) {
            const bool in_struct = random.below(4) != 0;
            const auto limit = static_cast<uint32_t>(in_struct ? std::min(sizeof(T), bytes.size()) : bytes.size());
            const uint32_t at = random.below(limit) & ~3U; // whole fields, mostly
            uint32_t value = 0;
            switch (random.below(6)) {
            case 0:
                value = 0;
                break;
            case 1:
                value = UINT32_MAX;
                break;
            case 2:
                value = static_cast<uint32_t>(bytes.size()); // the end of the file
                break;
            case 3:
                value = static_cast<uint32_t>(bytes.size()) - 4 * random.below(8);
                break;
            case 4:
                value = 1U << random.below(32);
                break;
            default:
                value = static_cast<uint32_t>(random.next());
                break;
            }
            if (at + 4 <= bytes.size()) {
                std::memcpy(bytes.data() + at, &value, 4);
            }
        }
        // Keep the header plausible half the time, so the body checks get exercised too.
        if (random.below(2) == 0) {
            AssetHeader header;
            std::memcpy(&header, original.data(), sizeof(header));
            header.content_hash = 0; // not checked on load
            AssetHeader corrupted;
            std::memcpy(&corrupted, bytes.data(), sizeof(corrupted));
            corrupted.magic = header.magic;
            corrupted.type = header.type;
            corrupted.format_version = header.format_version;
            corrupted.platform = header.platform;
            corrupted.file_size = header.file_size;
            corrupted.id = header.id;
            std::memcpy(bytes.data(), &corrupted, sizeof(corrupted));
        }
        const auto loaded = load_in_place<T>(bytes);
        if (loaded.ok()) {
            ++accepted;
            INFO("seed ", seed, " round ", round);
            REQUIRE(safe(bytes, **loaded));
        }
    }
    // Some corruptions land in data that isn't checked (texel blocks, vertex positions): those load.
    CHECK(accepted > 0);
}

std::vector<std::byte> to_vector(const de::core::Array<std::byte>& bytes) {
    return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> sample_mesh() {
    de::cooker::ModelData model;
    de::cooker::ModelPrimitive& primitive = model.primitives.emplace();
    constexpr uint32_t kSize = 8;
    for (uint32_t y = 0; y <= kSize; ++y) {
        for (uint32_t x = 0; x <= kSize; ++x) {
            de::cooker::ModelVertex& vertex = primitive.vertices.emplace();
            vertex.position[0] = static_cast<float>(x);
            vertex.position[2] = static_cast<float>(y);
            vertex.position[1] = 0.1f * static_cast<float>((x * 7 + y * 3) % 5);
        }
    }
    for (uint32_t y = 0; y < kSize; ++y) {
        for (uint32_t x = 0; x < kSize; ++x) {
            const uint32_t i = y * (kSize + 1) + x;
            for (const uint32_t index : {i, i + kSize + 1, i + 1, i + 1, i + kSize + 1, i + kSize + 2}) {
                primitive.indices.push(index);
            }
        }
    }
    de::cooker::Diagnostics diagnostics;
    const auto cooked = de::cooker::cook_mesh(model, {}, {}, AssetId::from_value(11), "fuzz", diagnostics);
    REQUIRE(cooked.ok());
    return to_vector(*cooked);
}

std::vector<std::byte> sample_texture() {
    de::cooker::Image image;
    image.width = 16;
    image.height = 8;
    for (uint32_t i = 0; i < image.width * image.height; ++i) {
        for (const uint8_t channel :
             {static_cast<uint8_t>(i * 3), static_cast<uint8_t>(i * 5), uint8_t{128}, uint8_t{255}}) {
            image.rgba.push(channel);
        }
    }
    de::cooker::Diagnostics diagnostics;
    const auto cooked = de::cooker::cook_texture(image, {}, AssetId::from_value(12), nullptr, "fuzz", diagnostics);
    REQUIRE(cooked.ok());
    return to_vector(*cooked);
}

std::vector<std::byte> sample_material() {
    de::cooker::MaterialBuild build;
    build.set_param(material::kRoughness, 0.5f);
    build.set_param(material::kBaseColor, 1.0f, 1.0f, 1.0f, 1.0f);
    build.set_texture(material::kNormalTexture, AssetId::from_value(2));
    build.set_texture(material::kBaseColorTexture, AssetId::from_value(3));
    return to_vector(de::cooker::cook_material(build, AssetId::from_value(13)));
}

// A cell with a parent, two meshes, and names.
std::vector<std::byte> sample_cell() {
    de::cooker::CellBuild build;
    build.meshes.push({.id = AssetId::from_value(21), .type = AssetType::Mesh});
    build.meshes.push({.id = AssetId::from_value(22), .type = AssetType::Mesh});
    const char names[] = "rootchildother";
    for (const char c : std::string_view(names)) {
        build.names.push(c);
    }
    build.entities.push({.position = {1.0, 2.0, 3.0}, .mesh = 0, .name_offset = 0, .name_length = 4});
    build.entities.push({.parent = 0, .mesh = 1, .name_offset = 4, .name_length = 5});
    build.fog_volumes.push({.density_per_m = 0.2f});
    build.entities.push({.parent = 1, .name_offset = 9, .name_length = 5, .fog_volume = 0});
    return to_vector(de::cooker::cook_cell(build, AssetId::from_value(14)));
}

// A shader file as the cooker lays it out, around stand-in SPIR-V.
std::vector<std::byte> sample_shader() {
    de::cooker::BlobWriter writer(sizeof(ShaderAsset));
    const uint32_t code[] = {kSpirvMagic, 0x00010600, 0, 16, 0};
    ShaderPushConstant field;
    std::memcpy(field.name, "color", 6);
    field.size = 16;
    ShaderEntryCode entries[2];
    for (ShaderEntryCode& entry : entries) {
        entry.code = writer.append(std::span<const uint32_t>(code));
        entry.push_constants = writer.append(std::span(&field, 1));
        entry.push_constant_bytes = 16;
    }
    std::memcpy(entries[0].entry, "vertex_main", 12);
    std::memcpy(entries[1].entry, "fragment_main", 14);
    entries[1].stage = ShaderStageKind::Fragment;
    const ShaderPermutation permutation{.defines_hash = 0,
                                        .entries = writer.append(std::span<const ShaderEntryCode>(entries))};
    ShaderAsset shader;
    std::memcpy(shader.module, "fuzz", 5);
    shader.permutations = writer.append(std::span(&permutation, 1));
    return to_vector(writer.finish(shader, shader_asset_id("fuzz"), {}));
}

} // namespace

TEST_CASE("corrupted cooked files are refused, or safe to use") {
    const std::vector<std::byte> mesh = sample_mesh();
    const std::vector<std::byte> texture = sample_texture();
    const std::vector<std::byte> material = sample_material();
    const std::vector<std::byte> shader = sample_shader();
    const std::vector<std::byte> cell = sample_cell();
    // The samples themselves load and are safe.
    Buffer mesh_file(mesh);
    REQUIRE(load_in_place<MeshAsset>(mesh_file.bytes()).ok());
    REQUIRE(safe(mesh_file.bytes(), **load_in_place<MeshAsset>(mesh_file.bytes())));
    Buffer shader_file(shader);
    REQUIRE(load_in_place<ShaderAsset>(shader_file.bytes()).ok());
    Buffer cell_file(cell);
    REQUIRE(load_in_place<CellAsset>(cell_file.bytes()).ok());

    constexpr int kRounds = 4000;
    fuzz<MeshAsset>(mesh, 1, kRounds);
    fuzz<TextureAsset>(texture, 2, kRounds);
    fuzz<MaterialAsset>(material, 3, kRounds);
    fuzz<ShaderAsset>(shader, 4, kRounds);
    fuzz<CellAsset>(cell, 5, kRounds);
}

TEST_CASE("textures whose sizes overflow the mip arithmetic are refused") {
    // A side near 2^32: (width + 3) / 4 wraps to 0 blocks, so a mip of 0 bytes would add up.
    de::cooker::BlobWriter writer(sizeof(TextureAsset));
    // Everything else in order: the mip lies in the file (after the struct, 16-byte aligned).
    const TextureMip mip{.offset = 96, .size = 0, .width = UINT32_MAX - 2, .height = 4, .row_bytes = 0, .rows = 1};
    TextureAsset texture;
    texture.format = TextureFormat::BC7Unorm;
    texture.width = UINT32_MAX - 2;
    texture.height = 4;
    texture.mips = writer.append(std::span(&mip, 1));
    const auto bytes = writer.finish(texture, AssetId::from_value(1), {});
    Buffer file(std::span(bytes.data(), bytes.size()));
    REQUIRE(file.bytes().size() >= 96);
    const auto loaded = load_in_place<TextureAsset>(file.bytes());
    REQUIRE_FALSE(loaded.ok());
    CHECK((std::string_view(loaded.error().message) ==
           "texture has no texels, more than 32768 on a side, or more mips than texels"));
}

TEST_CASE("shader code that isn't SPIR-V is refused before it reaches a driver") {
    std::vector<std::byte> shader = sample_shader();
    Buffer file(shader);
    const auto& asset = **load_in_place<ShaderAsset>(file.bytes());
    const ShaderPermutation& permutation = asset.permutations.get(file.bytes().data())[0];
    const uint32_t offset = permutation.entries.get(file.bytes().data())[0].code.offset;
    const uint32_t garbage = 0xdeadbeef;
    std::memcpy(file.bytes().data() + offset, &garbage, 4);
    CHECK(load_in_place<ShaderAsset>(file.bytes()).error().code == de::core::ErrorCode::CorruptData);
}
