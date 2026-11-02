#pragma once

#include "core/fixed_string.h"
#include "core/hash.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace de::asset {

// A source file's identity, kept in its meta file (docs/design.md, Asset formats): 128 random bits,
// minted once when the file first appears and never changed, so moves and renames keep references.
// Text form: "6f9619ff-8b86-d011-b42d-00c04fc964ff" (lowercase hex, 8-4-4-4-12).
struct Guid {
    uint64_t high = 0;
    uint64_t low = 0;

    constexpr bool is_valid() const { return high != 0 || low != 0; }
    constexpr bool operator==(const Guid&) const = default;
};

inline constexpr size_t kGuidTextLength = 36;
using GuidText = core::FixedString<kGuidTextLength>;

GuidText format_guid(Guid guid);
// Accepts exactly the text form, lowercase or uppercase hex. Fails (false) on anything else, or
// on the all-zero GUID.
bool parse_guid(std::string_view text, Guid& guid);

// What runtime code loads assets by: 64 bits derived from the GUID in the source's meta file, so a
// rename never changes it (docs/design.md, File system & IO). One source can cook to several
// assets (a glTF's mesh, its materials, its embedded images): the main one takes the GUID's ID,
// the others an ID derived from it and a name local to the source.
class AssetId {
public:
    constexpr AssetId() = default;

    static constexpr AssetId from_value(uint64_t value) {
        AssetId id;
        id.value_ = value;
        return id;
    }

    constexpr uint64_t value() const { return value_; }
    constexpr bool is_valid() const { return value_ != 0; }

    constexpr bool operator==(const AssetId&) const = default;
    constexpr bool operator<(const AssetId& other) const { return value_ < other.value_; }

private:
    uint64_t value_ = 0;
};

// Never 0 (the invalid ID): a derivation that lands on 0 takes 1 instead.
constexpr AssetId nonzero_id(uint64_t value) {
    return AssetId::from_value(value != 0 ? value : 1);
}

// The main asset of a source file.
constexpr AssetId asset_id(Guid guid) {
    return nonzero_id(core::mix64(guid.high ^ core::mix64(guid.low)));
}

// Another asset cooked from the same source: "material/stone", "image/2".
constexpr AssetId sub_asset_id(AssetId main, std::string_view name) {
    return nonzero_id(core::mix64(main.value() ^ core::fnv1a_64(name.data(), name.size())));
}

// An asset identified by a name instead of a GUID: shader modules, which code names directly
// ("shader", "imgui"), and which a rename breaks anyway.
constexpr AssetId named_asset_id(std::string_view kind, std::string_view name) {
    return nonzero_id(
        core::mix64(core::fnv1a_64(kind.data(), kind.size()) ^ core::mix64(core::fnv1a_64(name.data(), name.size()))));
}

inline constexpr size_t kAssetIdTextLength = 16;
using AssetIdText = core::FixedString<kAssetIdTextLength>;

// 16 lowercase hex digits, as in cooked file names and logs.
AssetIdText format_asset_id(AssetId id);
bool parse_asset_id(std::string_view text, AssetId& id);

} // namespace de::asset

template <>
struct de::core::Hash<de::asset::AssetId> {
    constexpr uint64_t operator()(de::asset::AssetId id) const { return id.value(); }
};
