#include "asset/asset_id.h"
#include "asset/format.h"
#include "asset/material_format.h"
#include "asset/mesh_format.h"
#include "asset/shader_format.h"
#include "asset/texture_format.h"
#include "cooker/blob_writer.h"
#include "cooker/material_cooker.h"

#include <doctest/doctest.h>

#include <cstring>

using namespace de::asset;
using de::cooker::MaterialBuild;

namespace {

// A cooked file in 16-byte aligned memory, as load_in_place needs; editable to corrupt it.
class AlignedFile {
public:
    explicit AlignedFile(const de::core::Array<std::byte>& bytes) : size_(bytes.size()) {
        words_.resize((bytes.size() + 15) / 16);
        std::memcpy(words_.data(), bytes.data(), bytes.size());
    }
    std::span<std::byte> bytes() { return {reinterpret_cast<std::byte*>(words_.data()), size_}; }
    AssetHeader& header() { return *reinterpret_cast<AssetHeader*>(words_.data()); }

private:
    struct alignas(16) Block {
        std::byte bytes[16];
    };
    de::core::Array<Block> words_;
    size_t size_;
};

// The bytes after offset filler bytes: copied into aligned memory, the file starts misaligned.
de::core::Array<std::byte> file_bytes_with_offset(std::span<const std::byte> bytes, size_t offset) {
    de::core::Array<std::byte> padded;
    padded.resize(offset);
    for (const std::byte byte : bytes) {
        padded.push(byte);
    }
    return padded;
}

de::core::Array<std::byte> sample_material(AssetId id) {
    MaterialBuild build;
    build.set_param(material::kRoughness, 0.25f);
    build.set_param(material::kBaseColor, 1.0f, 0.5f, 0.25f, 1.0f);
    build.set_texture(material::kNormalTexture, AssetId::from_value(42));
    build.set_texture(material::kBaseColorTexture, AssetId::from_value(7));
    return de::cooker::cook_material(build, id);
}

} // namespace

TEST_CASE("GUIDs read and write their text form, and asset IDs derive from them") {
    const Guid guid{.high = 0x6f9619ff8b864011ULL, .low = 0xb42d00c04fc964ffULL};
    CHECK((format_guid(guid).view() == "6f9619ff-8b86-4011-b42d-00c04fc964ff"));
    Guid parsed;
    REQUIRE(parse_guid("6F9619FF-8B86-4011-B42D-00C04FC964FF", parsed));
    CHECK(parsed == guid);
    Guid untouched = guid;
    CHECK_FALSE(parse_guid("6f9619ff-8b86-4011-b42d-00c04fc964f", untouched));  // short
    CHECK_FALSE(parse_guid("6f9619ff 8b86-4011-b42d-00c04fc964ff", untouched)); // no dash
    CHECK_FALSE(parse_guid("6f9619ff-8b86-4011-b42d-00c04fc964fg", untouched)); // not hex
    CHECK_FALSE(parse_guid("00000000-0000-0000-0000-000000000000", untouched)); // the null GUID
    CHECK(untouched == guid);

    // The main asset, its sub-assets and named assets: valid, distinct, stable.
    const AssetId main = asset_id(guid);
    CHECK(main.is_valid());
    CHECK(main == asset_id(parsed));
    CHECK(sub_asset_id(main, "material/stone") != main);
    CHECK(sub_asset_id(main, "material/stone") != sub_asset_id(main, "material/moss"));
    CHECK(sub_asset_id(main, "material/stone") == sub_asset_id(main, "material/stone"));
    CHECK(shader_asset_id("imgui") != shader_asset_id("triangle"));
    CHECK(named_asset_id("shader", "imgui") == shader_asset_id("imgui"));

    AssetId read_back;
    REQUIRE(parse_asset_id(format_asset_id(main).view(), read_back));
    CHECK(read_back == main);
    CHECK_FALSE(parse_asset_id("0000000000000000", read_back));
    const CookedPath path = cooked_path(AssetId::from_value(0x3f8a0c1e5b2d4e61ULL), AssetType::Mesh);
    CHECK((path.view() == "game:/3f/3f8a0c1e5b2d4e61.demesh"));
}

TEST_CASE("cooked files load in place, and broken ones are refused") {
    const AssetId id = AssetId::from_value(0x1234);
    AlignedFile file(sample_material(id));
    const auto loaded = load_in_place<MaterialAsset>(file.bytes());
    REQUIRE(loaded.ok());
    const MaterialAsset& material = **loaded;
    CHECK(material.header.id == id);
    CHECK(material.header.file_size == file.bytes().size());
    CHECK(file.bytes().size() % kAssetAlignment == 0);
    CHECK(verify_content_hash(file.bytes()));
    // Lookups by name, through the sorted tables.
    const MaterialParam* roughness = material.find_param(file.bytes().data(), material::kRoughness);
    REQUIRE(roughness != nullptr);
    CHECK(roughness->value[0] == 0.25f);
    CHECK(material.find_param(file.bytes().data(), material::kMetallic) == nullptr);
    CHECK(material.find_texture(file.bytes().data(), material::kNormalTexture) == AssetId::from_value(42));
    CHECK(material.find_texture(file.bytes().data(), material::kBaseColorTexture) == AssetId::from_value(7));
    CHECK_FALSE(material.find_texture(file.bytes().data(), material::kEmissiveTexture).is_valid());
    // The textures are its dependencies.
    const auto dependencies = material.header.dependencies.get(file.bytes().data());
    CHECK(dependencies.size() == 2);

    // Another type, version or platform.
    CHECK(load_in_place<MeshAsset>(file.bytes()).error().code == de::core::ErrorCode::InvalidArgument);
    file.header().format_version = MaterialAsset::kFormatVersion + 1;
    CHECK(load_in_place<MaterialAsset>(file.bytes()).error().code == de::core::ErrorCode::Unsupported);
    file.header().format_version = MaterialAsset::kFormatVersion;
    file.header().platform = Platform::Unknown;
    CHECK_FALSE(load_in_place<MaterialAsset>(file.bytes()).ok());
    file.header().platform = Platform::Pc;
    // Not a cooked file, or a truncated one.
    file.header().magic = 0;
    CHECK(load_in_place<MaterialAsset>(file.bytes()).error().code == de::core::ErrorCode::CorruptData);
    file.header().magic = kAssetMagic;
    CHECK(load_in_place<MaterialAsset>(file.bytes().first(file.bytes().size() - 16)).error().code ==
          de::core::ErrorCode::CorruptData);
    CHECK(load_in_place<MaterialAsset>(file.bytes().first(32)).error().code == de::core::ErrorCode::CorruptData);
    // An array pointing past the end.
    auto* body = reinterpret_cast<MaterialAsset*>(file.bytes().data());
    const OffsetSpan<MaterialParam> params = body->params;
    body->params.count = 1000;
    CHECK(load_in_place<MaterialAsset>(file.bytes()).error().code == de::core::ErrorCode::CorruptData);
    body->params = params;
    REQUIRE(load_in_place<MaterialAsset>(file.bytes()).ok());
    // Unaligned memory.
    const de::core::Array<std::byte> padded = file_bytes_with_offset(file.bytes(), 1);
    AlignedFile shifted(padded);
    CHECK(load_in_place<MaterialAsset>(shifted.bytes().subspan(1)).error().code ==
          de::core::ErrorCode::InvalidArgument);
    // A flipped bit anywhere after the header changes the content hash.
    file.bytes()[file.bytes().size() - 20] ^= std::byte{1};
    CHECK_FALSE(verify_content_hash(file.bytes()));
}

TEST_CASE("a texture in two material slots is one dependency") {
    MaterialBuild build;
    build.set_texture(material::kOcclusionTexture, AssetId::from_value(5));
    build.set_texture(material::kMetallicRoughnessTexture, AssetId::from_value(5));
    AlignedFile file(de::cooker::cook_material(build, AssetId::from_value(3)));
    const auto loaded = load_in_place<MaterialAsset>(file.bytes());
    REQUIRE(loaded.ok());
    CHECK((*loaded)->textures.count == 2);
    CHECK((*loaded)->header.dependencies.count == 1);
}

TEST_CASE("cooking the same input twice gives the same bytes") {
    const auto first = sample_material(AssetId::from_value(9));
    const auto second = sample_material(AssetId::from_value(9));
    REQUIRE(first.size() == second.size());
    CHECK(std::memcmp(first.data(), second.data(), first.size()) == 0);
}

TEST_CASE("texture formats know their block sizes") {
    CHECK(texture_block_dim(TextureFormat::BC7Srgb) == 4);
    CHECK(texture_block_bytes(TextureFormat::BC7Srgb) == 16);
    CHECK(texture_block_bytes(TextureFormat::BC1Unorm) == 8);
    CHECK(texture_block_bytes(TextureFormat::BC4Unorm) == 8);
    CHECK(texture_block_bytes(TextureFormat::BC5Unorm) == 16);
    CHECK(texture_block_dim(TextureFormat::RGBA8Srgb) == 1);
    CHECK(texture_block_bytes(TextureFormat::RGBA8Srgb) == 4);
    CHECK(texture_block_dim(TextureFormat::Unknown) == 0);
}

TEST_CASE("shader permutations are keyed by their defines in any order") {
    const ShaderDefineText ab[] = {{.name = "A", .value = "1"}, {.name = "B", .value = "2"}};
    const ShaderDefineText ba[] = {{.name = "B", .value = "2"}, {.name = "A", .value = "1"}};
    const ShaderDefineText a_default[] = {{.name = "A", .value = ""}};
    const ShaderDefineText a_one[] = {{.name = "A", .value = "1"}};
    const ShaderDefineText a_two[] = {{.name = "A", .value = "2"}};
    CHECK(shader_defines_hash({}) == 0);
    CHECK(shader_defines_hash(ab) == shader_defines_hash(ba));
    CHECK(shader_defines_hash(a_default) == shader_defines_hash(a_one));
    CHECK(shader_defines_hash(a_one) != shader_defines_hash(a_two));
    CHECK(shader_defines_hash(a_one) != 0);
}
