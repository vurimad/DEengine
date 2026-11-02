#include "cooker/settings.h"

#include <algorithm>

namespace de::cooker {

const char* texture_usage_name(TextureUsage usage) {
    switch (usage) {
    case TextureUsage::Color:
        return "color";
    case TextureUsage::Normal:
        return "normal";
    case TextureUsage::Data:
        return "data";
    case TextureUsage::Unknown:
        break;
    }
    return "unknown";
}

bool parse_texture_usage(std::string_view text, TextureUsage& usage) {
    for (const TextureUsage candidate : {TextureUsage::Color, TextureUsage::Normal, TextureUsage::Data}) {
        if (text == texture_usage_name(candidate)) {
            usage = candidate;
            return true;
        }
    }
    return false;
}

void texture_usage_defaults(TextureUsage usage, JsonBuilder& settings) {
    yyjson_mut_doc* doc = settings.doc();
    yyjson_mut_val* root = settings.root();
    switch (usage) {
    case TextureUsage::Normal:
        yyjson_mut_obj_add_str(doc, root, "compression", "bc5");
        yyjson_mut_obj_add_bool(doc, root, "srgb", false);
        yyjson_mut_obj_add_bool(doc, root, "normal_map", true);
        break;
    case TextureUsage::Data:
        yyjson_mut_obj_add_str(doc, root, "compression", "bc7");
        yyjson_mut_obj_add_bool(doc, root, "srgb", false);
        break;
    case TextureUsage::Color:
    case TextureUsage::Unknown:
        yyjson_mut_obj_add_str(doc, root, "compression", "bc7");
        yyjson_mut_obj_add_bool(doc, root, "srgb", true);
        break;
    }
}

TextureSettings read_texture_settings(yyjson_val* section, std::string_view path, Diagnostics& diagnostics) {
    TextureSettings settings;
    if (section == nullptr) {
        return settings;
    }
    const JsonReader reader{.object = section, .path = path, .diagnostics = &diagnostics};
    static constexpr const char* kKeys[] = {"compression", "srgb", "mips", "normal_map", "max_size"};
    reader.check_keys(kKeys, "texture");
    struct Name {
        std::string_view text;
        TextureCompression compression;
    };
    static constexpr Name kCompressions[] = {
        {.text = "bc7", .compression = TextureCompression::BC7},
        {.text = "bc5", .compression = TextureCompression::BC5},
        {.text = "bc4", .compression = TextureCompression::BC4},
        {.text = "bc1", .compression = TextureCompression::BC1},
        {.text = "none", .compression = TextureCompression::None},
    };
    const std::string_view compression = reader.string("compression", "bc7");
    bool known = false;
    for (const Name& name : kCompressions) {
        if (name.text == compression) {
            settings.compression = name.compression;
            known = true;
        }
    }
    if (!known) {
        diagnostics.error(path, "texture compression '%.*s' isn't one of bc7, bc5, bc4, bc1, none",
                          static_cast<int>(compression.size()), compression.data());
    }
    settings.srgb = reader.boolean("srgb", settings.srgb);
    settings.mips = reader.boolean("mips", settings.mips);
    settings.normal_map = reader.boolean("normal_map", settings.normal_map);
    const double max_size = reader.number("max_size", 0.0);
    if (max_size < 0.0 || max_size > 32768.0 || max_size != static_cast<double>(static_cast<uint32_t>(max_size))) {
        diagnostics.error(path, "texture max_size should be a whole number of texels from 0 (no limit) to 32768");
    } else {
        settings.max_size = static_cast<uint32_t>(max_size);
    }
    if (settings.srgb && (settings.compression == TextureCompression::BC4 ||
                          settings.compression == TextureCompression::BC5 || settings.normal_map)) {
        diagnostics.error(path, "srgb is for color textures: bc4, bc5 and normal maps hold linear data");
        settings.srgb = false;
    }
    return settings;
}

MeshSettings read_mesh_settings(yyjson_val* section, std::string_view path, Diagnostics& diagnostics) {
    MeshSettings settings;
    if (section == nullptr) {
        return settings;
    }
    const JsonReader reader{.object = section, .path = path, .diagnostics = &diagnostics};
    static constexpr const char* kKeys[] = {"scale", "lod_count", "lod_ratio", "lod_error", "optimize"};
    reader.check_keys(kKeys, "mesh");
    const double scale = reader.number("scale", settings.scale);
    const double lod_count = reader.number("lod_count", settings.lod_count);
    const double lod_ratio = reader.number("lod_ratio", settings.lod_ratio);
    const double lod_error = reader.number("lod_error", settings.lod_error);
    if (!(scale > 0.0)) {
        diagnostics.error(path, "mesh scale should be above 0");
    } else {
        settings.scale = static_cast<float>(scale);
    }
    if (lod_count < 1.0 || lod_count > 8.0 || lod_count != static_cast<double>(static_cast<uint32_t>(lod_count))) {
        diagnostics.error(path, "mesh lod_count should be a whole number from 1 to 8");
    } else {
        settings.lod_count = static_cast<uint32_t>(lod_count);
    }
    if (!(lod_ratio > 0.0 && lod_ratio < 1.0)) {
        diagnostics.error(path, "mesh lod_ratio should be between 0 and 1");
    } else {
        settings.lod_ratio = static_cast<float>(lod_ratio);
    }
    if (!(lod_error > 0.0 && lod_error <= 1.0)) {
        diagnostics.error(path, "mesh lod_error should be above 0 and at most 1");
    } else {
        settings.lod_error = static_cast<float>(lod_error);
    }
    settings.optimize = reader.boolean("optimize", settings.optimize);
    return settings;
}

} // namespace de::cooker
