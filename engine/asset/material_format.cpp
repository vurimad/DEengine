#include "asset/material_format.h"

#include <algorithm>

namespace de::asset {

namespace {

template <typename T, typename Key>
bool sorted_unique(std::span<const T> items, Key key) {
    for (size_t i = 1; i < items.size(); ++i) {
        if (!(key(items[i - 1]) < key(items[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

core::Result<void> MaterialAsset::validate(std::span<const std::byte> file) const {
    using core::ErrorCode;
    if (!span_in_file(params, file.size()) || !span_in_file(textures, file.size())) {
        return core::fail(ErrorCode::CorruptData, "material array lies outside the file");
    }
    if (alpha_mode != AlphaMode::Opaque && alpha_mode != AlphaMode::Masked && alpha_mode != AlphaMode::Blend) {
        return core::fail(ErrorCode::CorruptData, "material has an unknown alpha mode");
    }
    if (!sorted_unique(params.get(file.data()), [](const MaterialParam& param) { return param.name; }) ||
        !sorted_unique(textures.get(file.data()), [](const MaterialTexture& texture) { return texture.slot; })) {
        return core::fail(ErrorCode::CorruptData, "material names aren't sorted and unique");
    }
    return {};
}

const MaterialParam* MaterialAsset::find_param(const void* file, core::StringId name) const {
    const std::span<const MaterialParam> all = params.get(file);
    const auto* found =
        std::lower_bound(all.data(), all.data() + all.size(), name,
                         [](const MaterialParam& param, core::StringId key) { return param.name < key; });
    return found != all.data() + all.size() && found->name == name ? found : nullptr;
}

AssetId MaterialAsset::find_texture(const void* file, core::StringId slot) const {
    const std::span<const MaterialTexture> all = textures.get(file);
    const auto* found =
        std::lower_bound(all.data(), all.data() + all.size(), slot,
                         [](const MaterialTexture& texture, core::StringId key) { return texture.slot < key; });
    return found != all.data() + all.size() && found->slot == slot ? found->texture : AssetId();
}

} // namespace de::asset
