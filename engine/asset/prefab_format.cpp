#include "asset/prefab_format.h"

#include "core/hash.h"

#include <cmath>

namespace de::asset {

uint32_t prefab_variant_hash(std::string_view name) {
    if (name.empty()) {
        return 0;
    }
    const auto hash = static_cast<uint32_t>(core::fnv1a_64(name.data(), name.size()));
    return hash == 0 ? 1 : hash; // 0 is kept for no variant
}

uint64_t prefab_part_id(std::string_view path) {
    return core::mix64(core::fnv1a_64(path.data(), path.size()));
}

uint32_t PrefabAsset::variant_bit(const void* file, uint32_t name_hash) const {
    if (name_hash == 0) {
        return 0;
    }
    const std::span<const PrefabVariant> all = variants.get(file);
    for (size_t v = 0; v < all.size(); ++v) {
        if (all[v].name_hash == name_hash) {
            return 1U << static_cast<uint32_t>(v);
        }
    }
    return 0;
}

core::Result<void> PrefabAsset::validate(std::span<const std::byte> file) const {
    using core::ErrorCode;
    if (!span_in_file(parts, file.size()) || !span_in_file(variants, file.size()) ||
        !span_in_file(names, file.size()) || !span_in_file(lights, file.size())) {
        return core::fail(ErrorCode::CorruptData, "prefab array lies outside the file");
    }
    if (parts.count == 0 || parts.count > PrefabLimits::kMaxParts || variants.count > PrefabLimits::kMaxVariants) {
        return core::fail(ErrorCode::CorruptData, "prefab has no parts, or too many parts or variants");
    }
    if (!std::isfinite(reach_m) || reach_m < 0.0f || reach_m > PrefabLimits::kMaxReachM) {
        return core::fail(ErrorCode::CorruptData, "prefab's reach is out of range");
    }
    const std::span<const AssetRef> dependencies = header.dependencies.get(file.data());
    const uint32_t variant_bits =
        variants.count == 32 ? UINT32_MAX : (1U << variants.count) - 1U; // the bits a part may name
    const auto names_hold = [this](uint32_t offset, uint16_t length) {
        return static_cast<uint64_t>(offset) + length <= names.count;
    };
    const auto depends = [&dependencies](uint32_t index, AssetType type) {
        return index == PrefabPart::kNone || (index < dependencies.size() && dependencies[index].type == type);
    };
    for (const PrefabVariant& variant : variants.get(file.data())) {
        if (variant.name_length == 0 || !names_hold(variant.name_offset, variant.name_length) ||
            variant.name_hash == 0) {
            return core::fail(ErrorCode::CorruptData, "prefab variant has no name, or one outside the names");
        }
    }
    for (const LightDesc& light : lights.get(file.data())) {
        if (!light.valid()) {
            return core::fail(ErrorCode::CorruptData, "prefab light has bad values");
        }
    }
    for (const PrefabPart& part : parts.get(file.data())) {
        if (part.name_length == 0 || !names_hold(part.name_offset, part.name_length)) {
            return core::fail(ErrorCode::CorruptData, "prefab part has no name, or one outside the names");
        }
        if (part.light != PrefabPart::kNone && part.light >= lights.count) {
            return core::fail(ErrorCode::CorruptData, "prefab part's light isn't in the prefab");
        }
        if (!depends(part.mesh, AssetType::Mesh) || !depends(part.collision, AssetType::Collision) ||
            !depends(part.effect, AssetType::Effect)) {
            return core::fail(ErrorCode::CorruptData, "prefab part's asset isn't one of the prefab's dependencies");
        }
        if ((part.absent_in & ~variant_bits) != 0) {
            return core::fail(ErrorCode::CorruptData, "prefab part names a variant the prefab doesn't have");
        }
        bool placed = true;
        for (const float value : part.position) {
            placed = placed && std::isfinite(value) && std::abs(value) <= PrefabLimits::kMaxReachM;
        }
        for (const float value : part.rotation) {
            placed = placed && std::isfinite(value);
        }
        for (const float value : part.scale) {
            placed = placed && std::isfinite(value) && value != 0.0f;
        }
        if (!placed) {
            return core::fail(ErrorCode::CorruptData, "prefab part's transform isn't finite, or its scale has a zero");
        }
    }
    return {};
}

} // namespace de::asset
