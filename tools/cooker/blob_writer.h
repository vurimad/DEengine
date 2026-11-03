#pragma once

#include "asset/format.h"
#include "cooker/common.h"

#include <cstring>
#include <span>
#include <type_traits>

namespace de::cooker {

// Builds a cooked file: the asset struct first, then its arrays, each 16-byte aligned, addressed by
// offsets from the start (load-in-place). Fill the struct's spans from append(), then finish().
//     BlobWriter writer(sizeof(MeshAsset));
//     mesh.vertices = writer.append(vertices.span());
//     core::Array<std::byte> file = writer.finish(mesh, id, dependencies);
class BlobWriter {
public:
    explicit BlobWriter(size_t asset_size) { bytes_.resize(asset_size); }

    template <typename T>
    asset::OffsetSpan<std::remove_const_t<T>> append(std::span<T> items) {
        static_assert(std::is_trivially_copyable_v<T>);
        const uint32_t offset = append_bytes(std::as_bytes(items));
        return {.offset = items.empty() ? 0 : offset, .count = static_cast<uint32_t>(items.size())};
    }

    // Raw bytes (mip data), 16-byte aligned. Returns their offset.
    uint32_t append_bytes(std::span<const std::byte> data);

    // Writes the asset struct and the common header (type, version, ID, platform, dependencies, size,
    // content hash) and returns the file. T is a *Asset struct whose header is its first member.
    template <typename T>
    core::Array<std::byte> finish(T asset, asset::AssetId id, std::span<const asset::AssetRef> dependencies) {
        static_assert(std::is_trivially_copyable_v<T> && offsetof(T, header) == 0);
        DE_ASSERT(sizeof(T) <= bytes_.size());
        asset.header = asset::AssetHeader{};
        asset.header.type = T::kType;
        asset.header.format_version = T::kFormatVersion;
        asset.header.platform = asset::Platform::Pc;
        asset.header.id = id;
        asset.header.dependencies = append(dependencies);
        std::memcpy(bytes_.data(), &asset, sizeof(T));
        return seal();
    }

private:
    core::Array<std::byte> seal();

    core::Array<std::byte> bytes_{tools_heap()};
};

} // namespace de::cooker
