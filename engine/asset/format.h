#pragma once

#include "asset/asset_id.h"
#include "core/result.h"

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>

namespace de::asset {

// Cooked files are read straight into memory and used as they are (docs/design.md, Asset formats:
// load-in-place), so their layout is the in-memory layout of the structs below, on little-endian
// machines.
static_assert(std::endian::native == std::endian::little, "cooked files are little-endian");

enum class AssetType : uint16_t {
    Unknown = 0,
    Mesh = 1,     // .demesh
    Texture = 2,  // .detex
    Material = 3, // .demat
    Shader = 4,   // .deshader
    Cell = 5,     // .decell
};

// ".demesh"; "" for Unknown.
const char* asset_type_extension(AssetType type);
// "mesh"; "unknown" for Unknown.
const char* asset_type_name(AssetType type);

// What a file was cooked for: texture compression, shader bytecode.
enum class Platform : uint8_t {
    Unknown = 0,
    Pc = 1, // Vulkan on Windows and Linux: BCn textures, SPIR-V
};

inline constexpr uint32_t kAssetMagic = 0x53414544; // "DEAS"
// Cooked files are read into memory aligned to this, and every array in them starts on it.
inline constexpr size_t kAssetAlignment = 16;
// Files are addressed with 32-bit offsets.
inline constexpr uint64_t kMaxAssetFileSize = UINT32_MAX;

// An array inside a cooked file: count elements of T at offset bytes from the start of the file.
// Offsets, not pointers, so the file needs no fixing up: it can be read anywhere and used at once.
template <typename T>
struct OffsetSpan {
    uint32_t offset = 0;
    uint32_t count = 0;

    // file: the start of the cooked file. Only after the file has been validated (load_in_place).
    std::span<const T> get(const void* file) const {
        return {reinterpret_cast<const T*>(static_cast<const std::byte*>(file) + offset), count};
    }
};

// An asset another one needs: its ID, and its type, which names its file (cooked_path).
struct AssetRef {
    AssetId id;
    AssetType type = AssetType::Unknown;
    uint8_t reserved[6] = {};

    constexpr bool operator==(const AssetRef& other) const { return id == other.id && type == other.type; }
};
static_assert(sizeof(AssetRef) == 16);

// Starts every cooked file (docs/design.md, Asset formats: common header).
struct AssetHeader {
    uint32_t magic = kAssetMagic;
    AssetType type = AssetType::Unknown;
    uint16_t format_version = 0; // per type; a change means a re-cook, old files are never patched
    Platform platform = Platform::Unknown;
    uint8_t reserved0[7] = {};
    uint64_t file_size = 0;
    AssetId id;
    // Of every byte after this header: equal hashes mean equal assets (archives share them).
    uint64_t content_hash = 0;
    // Assets this one needs loaded with it (a material's textures, a mesh's materials).
    OffsetSpan<AssetRef> dependencies;
    uint8_t reserved1[16] = {};
};
static_assert(sizeof(AssetHeader) == 64);

// Whether span lies inside a file of file_size bytes, after the header, aligned for T. An empty span
// is always valid (and never read).
template <typename T>
bool span_in_file(const OffsetSpan<T>& span, uint64_t file_size) {
    if (span.count == 0) {
        return true;
    }
    const uint64_t end = static_cast<uint64_t>(span.offset) + static_cast<uint64_t>(span.count) * sizeof(T);
    return span.offset >= sizeof(AssetHeader) && span.offset % alignof(T) == 0 && end <= file_size;
}

// The checks every format shares: alignment of the memory, magic, type, version, platform, size, and
// the dependency list (inside the file, every entry a valid ID of a known type).
core::Result<void> validate_header(std::span<const std::byte> file, AssetType type, uint16_t format_version);

// The hash a header stores for these bytes.
uint64_t content_hash(std::span<const std::byte> file);

// Whether the file's bytes still hash to its header's content_hash: a check for corruption
// (a full pass over the file, so not done on every load).
bool verify_content_hash(std::span<const std::byte> file);

// Where the cooked file of an asset lives: "game:/3f/3f8a0c1e5b2d4e61.demesh". Files are named by
// ID, since runtime code loads by ID; the cooker's state maps IDs back to source paths for tools.
// The two-digit folder keeps folders small.
using CookedPath = core::FixedString<48>;
CookedPath cooked_path(AssetId id, AssetType type);

// Checks a cooked file and returns its contents, used where they lie: no parsing, no copies
// (docs/design.md, Asset formats). T is one of the *Asset structs (MeshAsset...): the header must
// match T's type and version, and every array in it must lie inside the file. The memory must stay
// valid, and unchanged, as long as the result is used.
template <typename T>
core::Result<const T*> load_in_place(std::span<const std::byte> file) {
    if (const core::Result<void> header = validate_header(file, T::kType, T::kFormatVersion); !header) {
        return core::fail(header.error());
    }
    if (file.size() < sizeof(T)) {
        return core::fail(core::ErrorCode::CorruptData, "cooked file is shorter than its header");
    }
    const T* asset = reinterpret_cast<const T*>(file.data());
    if (const core::Result<void> body = asset->validate(file); !body) {
        return core::fail(body.error());
    }
    return asset;
}

} // namespace de::asset

template <>
struct de::core::Hash<de::asset::AssetRef> {
    constexpr uint64_t operator()(const de::asset::AssetRef& ref) const {
        return mix64(ref.id.value() ^ (static_cast<uint64_t>(ref.type) << 56U));
    }
};
