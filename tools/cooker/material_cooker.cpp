#include "cooker/material_cooker.h"

#include "cooker/blob_writer.h"
#include "cooker/source_tree.h"
#include "platform/vfs.h"

#include <algorithm>

namespace de::cooker {

namespace material = asset::material;

void MaterialBuild::set_param(core::StringId name, float x, float y, float z, float w) {
    for (asset::MaterialParam& param : params) {
        if (param.name == name) {
            param = {.name = name, .value = {x, y, z, w}};
            return;
        }
    }
    params.push({.name = name, .value = {x, y, z, w}});
}

void MaterialBuild::set_texture(core::StringId slot, asset::AssetId texture) {
    for (asset::MaterialTexture& existing : textures) {
        if (existing.slot == slot) {
            existing.texture = texture;
            return;
        }
    }
    textures.push({.slot = slot, .texture = texture});
}

core::Array<std::byte> cook_material(MaterialBuild& build, asset::AssetId id) {
    std::ranges::sort(build.params,
                      [](const asset::MaterialParam& a, const asset::MaterialParam& b) { return a.name < b.name; });
    std::ranges::sort(build.textures,
                      [](const asset::MaterialTexture& a, const asset::MaterialTexture& b) { return a.slot < b.slot; });
    core::Array<asset::AssetId> dependencies(tools_heap());
    for (const asset::MaterialTexture& texture : build.textures) {
        dependencies.push(texture.texture);
    }
    asset::MaterialAsset asset;
    asset.shader_template = build.shader_template;
    asset.alpha_mode = build.alpha_mode;
    asset.flags = build.flags;
    BlobWriter writer(sizeof(asset::MaterialAsset));
    asset.params = writer.append(build.params.span());
    asset.textures = writer.append(build.textures.span());
    return writer.finish(asset, id, dependencies.span());
}

core::StringId default_template(asset::AlphaMode mode) {
    switch (mode) {
    case asset::AlphaMode::Masked:
        return material::kPbrMasked;
    case asset::AlphaMode::Blend:
        return material::kPbrBlend;
    case asset::AlphaMode::Opaque:
        break;
    }
    return material::kPbrOpaque;
}

MaterialBuild material_from_model(const ModelMaterial& source, std::span<const asset::AssetId> texture_ids) {
    MaterialBuild build;
    build.alpha_mode = source.alpha_mode;
    build.shader_template = default_template(source.alpha_mode);
    build.flags = source.double_sided ? asset::MaterialFlags::DoubleSided : asset::MaterialFlags::None;
    build.set_param(material::kBaseColor, source.base_color[0], source.base_color[1], source.base_color[2],
                    source.base_color[3]);
    build.set_param(material::kEmissive, source.emissive[0], source.emissive[1], source.emissive[2]);
    build.set_param(material::kMetallic, source.metallic);
    build.set_param(material::kRoughness, source.roughness);
    build.set_param(material::kNormalScale, source.normal_scale);
    build.set_param(material::kOcclusionStrength, source.occlusion_strength);
    if (source.alpha_mode == asset::AlphaMode::Masked) {
        build.set_param(material::kAlphaCutoff, source.alpha_cutoff);
    }
    const auto texture = [&](core::StringId slot, uint32_t index) {
        if (index < texture_ids.size() && texture_ids[index].is_valid()) {
            build.set_texture(slot, texture_ids[index]);
        }
    };
    texture(material::kBaseColorTexture, source.base_color_texture);
    texture(material::kNormalTexture, source.normal_texture);
    texture(material::kMetallicRoughnessTexture, source.metallic_roughness_texture);
    texture(material::kOcclusionTexture, source.occlusion_texture);
    texture(material::kEmissiveTexture, source.emissive_texture);
    return build;
}

core::Result<MaterialBuild> import_text_material(const platform::Vfs& vfs, const SourceTree& tree, const Source& source,
                                                 core::Array<TextureUse>& uses, Diagnostics& diagnostics) {
    const std::string_view path = source.path.view();
    const core::Result<core::Array<std::byte>> text = read_whole_file(vfs, src_path(path).c_str());
    if (!text) {
        diagnostics.error(path, "could not be read: %s", text.error().message);
        return core::fail(text.error());
    }
    const core::Result<JsonDoc> doc = JsonDoc::parse(text->span(), path, diagnostics);
    if (!doc) {
        return core::fail(doc.error());
    }
    const size_t errors_before = diagnostics.error_count();
    const JsonReader reader{.object = doc->root(), .path = path, .diagnostics = &diagnostics};
    static constexpr const char* kKeys[] = {"template", "alpha_mode", "double_sided", "params", "textures"};
    reader.check_keys(kKeys, "material");

    MaterialBuild build;
    const std::string_view alpha = reader.string("alpha_mode", "opaque");
    if (alpha == "masked") {
        build.alpha_mode = asset::AlphaMode::Masked;
    } else if (alpha == "blend") {
        build.alpha_mode = asset::AlphaMode::Blend;
    } else if (alpha != "opaque") {
        diagnostics.error(path, "alpha_mode '%.*s' isn't one of opaque, masked, blend", static_cast<int>(alpha.size()),
                          alpha.data());
    }
    const std::string_view template_name = reader.string("template", "");
    build.shader_template = template_name.empty() ? default_template(build.alpha_mode)
                                                  : core::StringId(template_name.data(), template_name.size());
    build.flags =
        reader.boolean("double_sided", false) ? asset::MaterialFlags::DoubleSided : asset::MaterialFlags::None;

    size_t index = 0;
    size_t count = 0;
    yyjson_val* key = nullptr;
    yyjson_val* value = nullptr;
    yyjson_obj_foreach(yyjson_obj_get(doc->root(), "params"), index, count, key, value) {
        float values[4] = {};
        size_t components = 0;
        if (yyjson_is_num(value)) {
            values[0] = static_cast<float>(yyjson_get_num(value));
            components = 1;
        } else if (yyjson_is_arr(value) && yyjson_arr_size(value) >= 1 && yyjson_arr_size(value) <= 4) {
            size_t i = 0;
            size_t n = 0;
            yyjson_val* item = nullptr;
            yyjson_arr_foreach(value, i, n, item) {
                if (yyjson_is_num(item)) {
                    values[components++] = static_cast<float>(yyjson_get_num(item));
                }
            }
            components = components == yyjson_arr_size(value) ? components : 0;
        }
        if (components == 0) {
            diagnostics.error(path, "param '%s' should be a number or an array of 1 to 4 numbers", yyjson_get_str(key));
            continue;
        }
        build.set_param(core::StringId(yyjson_get_str(key), yyjson_get_len(key)), values[0], values[1], values[2],
                        values[3]);
    }
    yyjson_obj_foreach(yyjson_obj_get(doc->root(), "textures"), index, count, key, value) {
        const std::string_view slot(yyjson_get_str(key), yyjson_get_len(key));
        SourcePath texture_path;
        const Source* texture = nullptr;
        if (yyjson_is_str(value) &&
            join_relative(folder_of(path), {yyjson_get_str(value), yyjson_get_len(value)}, texture_path)) {
            texture = tree.find(texture_path.view());
        }
        if (texture == nullptr || texture->kind != SourceKind::Texture) {
            diagnostics.error(path, "texture '%.*s' should name a texture source, relative to the material's folder",
                              static_cast<int>(slot.size()), slot.data());
            continue;
        }
        build.set_texture(core::StringId(slot.data(), slot.size()), texture->id);
        const TextureUsage usage = slot == "normal"                             ? TextureUsage::Normal
                                   : slot == "base_color" || slot == "emissive" ? TextureUsage::Color
                                                                                : TextureUsage::Data;
        uses.push(TextureUse{.path = texture_path, .usage = usage});
    }
    if (diagnostics.error_count() != errors_before) {
        return core::fail(core::ErrorCode::InvalidArgument, "the material has errors");
    }
    return build;
}

} // namespace de::cooker
