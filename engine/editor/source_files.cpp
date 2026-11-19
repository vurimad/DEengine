#include "editor/source_files.h"

#include "core/fixed_string.h"
#include "platform/vfs.h"

#include <cstddef>
#include <span>

namespace de::editor {

namespace {

struct Kind {
    std::string_view extension;
    std::string_view name;
};

constexpr Kind kKinds[] = {
    {.extension = ".world", .name = "World"},
    {.extension = ".cell", .name = "Cell"},
    {.extension = ".environment", .name = "Environment"},
    {.extension = ".terrain", .name = "Terrain"},
    {.extension = ".terrainlayers", .name = "Terrain layers"},
    {.extension = ".heightpng", .name = "Height map"},
    {.extension = ".material", .name = "Material"},
    {.extension = ".vfx", .name = "Effect"},
    {.extension = ".prefab", .name = "Prefab"},
    {.extension = ".png", .name = "Texture"},
    {.extension = ".jpg", .name = "Texture"},
    {.extension = ".tga", .name = "Texture"},
    {.extension = ".exr", .name = "Texture"},
    {.extension = ".hdr", .name = "Texture"},
    {.extension = ".gltf", .name = "Model"},
    {.extension = ".glb", .name = "Model"},
    {.extension = ".fbx", .name = "Model"},
    {.extension = ".views", .name = "Camera views"},
    {.extension = ".json", .name = "Data"},
    {.extension = ".lua", .name = "Script"},
};

constexpr std::string_view kContent = "src:/content/";

} // namespace

std::string_view file_kind(std::string_view name) {
    for (const Kind& kind : kKinds) {
        if (name.ends_with(kind.extension)) {
            return kind.name;
        }
    }
    const size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string_view("File") : name.substr(dot);
}

bool source_asset_id(const platform::Vfs& vfs, std::string_view content_path, asset::Guid& guid, asset::AssetId& id) {
    if (!content_path.starts_with(kContent)) {
        return false;
    }
    core::FixedString<300> meta;
    if (!meta.assign("src:/meta/") || !meta.append(content_path.substr(kContent.size())) || !meta.append(".json")) {
        return false;
    }
    // A meta is a few lines: {"guid": "8-4-4-4-12"} and maybe import settings.
    std::byte bytes[2048];
    const auto read = vfs.read_file(meta.c_str(), bytes);
    if (!read) {
        return false;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes), *read);
    const size_t key = text.find("\"guid\"");
    if (key == std::string_view::npos) {
        return false;
    }
    const size_t open = text.find('"', text.find(':', key));
    const size_t close = open == std::string_view::npos ? open : text.find('"', open + 1);
    if (close == std::string_view::npos || !asset::parse_guid(text.substr(open + 1, close - open - 1), guid)) {
        return false;
    }
    id = asset::asset_id(guid);
    return true;
}

} // namespace de::editor
