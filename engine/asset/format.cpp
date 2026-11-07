#include "asset/format.h"

#include "core/hash.h"

#include <cstring>

namespace de::asset {

const char* asset_type_extension(AssetType type) {
    switch (type) {
    case AssetType::Mesh:
        return ".demesh";
    case AssetType::Texture:
        return ".detex";
    case AssetType::Material:
        return ".demat";
    case AssetType::Shader:
        return ".deshader";
    case AssetType::Cell:
        return ".decell";
    case AssetType::World:
        return ".deworld";
    case AssetType::Environment:
        return ".deenv";
    case AssetType::Terrain:
        return ".deterrain";
    case AssetType::TerrainHeights:
        return ".deheights";
    case AssetType::Unknown:
        break;
    }
    return "";
}

const char* asset_type_name(AssetType type) {
    switch (type) {
    case AssetType::Mesh:
        return "mesh";
    case AssetType::Texture:
        return "texture";
    case AssetType::Material:
        return "material";
    case AssetType::Shader:
        return "shader";
    case AssetType::Cell:
        return "cell";
    case AssetType::World:
        return "world";
    case AssetType::Environment:
        return "environment";
    case AssetType::Terrain:
        return "terrain";
    case AssetType::TerrainHeights:
        return "terrain heights";
    case AssetType::Unknown:
        break;
    }
    return "unknown";
}

core::Result<void> validate_header(std::span<const std::byte> file, AssetType type, uint16_t format_version) {
    using core::ErrorCode;
    if (reinterpret_cast<uintptr_t>(file.data()) % kAssetAlignment != 0) {
        return core::fail(ErrorCode::InvalidArgument, "cooked files must be loaded at 16-byte aligned addresses");
    }
    if (file.size() < sizeof(AssetHeader)) {
        return core::fail(ErrorCode::CorruptData, "cooked file is shorter than its header");
    }
    AssetHeader header;
    std::memcpy(&header, file.data(), sizeof(header));
    if (header.magic != kAssetMagic) {
        return core::fail(ErrorCode::CorruptData, "not a cooked DEengine file");
    }
    if (header.type != type) {
        return core::fail(ErrorCode::InvalidArgument, "cooked file holds another type of asset");
    }
    if (header.format_version != format_version) {
        return core::fail(ErrorCode::Unsupported, "cooked file has another format version: cook it again");
    }
    if (header.platform != Platform::Pc) {
        return core::fail(ErrorCode::Unsupported, "cooked file is for another platform");
    }
    if (header.file_size != file.size()) {
        return core::fail(ErrorCode::CorruptData, "cooked file size differs from its header (truncated?)");
    }
    if (!header.id.is_valid()) {
        return core::fail(ErrorCode::CorruptData, "cooked file has no asset ID");
    }
    if (!span_in_file(header.dependencies, header.file_size)) {
        return core::fail(ErrorCode::CorruptData, "cooked file's dependency list lies outside it");
    }
    for (const AssetRef& dependency : header.dependencies.get(file.data())) {
        if (!dependency.id.is_valid() || asset_type_extension(dependency.type)[0] == '\0') {
            return core::fail(ErrorCode::CorruptData, "cooked file depends on an invalid ID or an unknown type");
        }
    }
    return {};
}

uint64_t content_hash(std::span<const std::byte> file) {
    if (file.size() <= sizeof(AssetHeader)) {
        return core::hash_bytes(nullptr, 0);
    }
    return core::hash_bytes(file.data() + sizeof(AssetHeader), file.size() - sizeof(AssetHeader));
}

bool verify_content_hash(std::span<const std::byte> file) {
    if (file.size() < sizeof(AssetHeader)) {
        return false;
    }
    AssetHeader header;
    std::memcpy(&header, file.data(), sizeof(header));
    return header.content_hash == content_hash(file);
}

CookedPath cooked_path(AssetId id, AssetType type) {
    const AssetIdText name = format_asset_id(id);
    CookedPath path("game:/");
    // Always fits: 6 + 2 + 1 + 16 + at most 9 characters.
    const bool fits = path.append(name.view().substr(0, 2)) && path.append('/') && path.append(name.view()) &&
                      path.append(asset_type_extension(type));
    DE_ASSERT(fits);
    (void)fits;
    return path;
}

} // namespace de::asset
