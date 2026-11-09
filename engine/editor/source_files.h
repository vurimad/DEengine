#pragma once

#include "asset/asset_id.h"

#include <string_view>

namespace de::platform {
class Vfs;
}

namespace de::editor {

// What a source file is, for people, by its extension: "World", "Texture", "Model"... or the extension itself.
std::string_view file_kind(std::string_view name);

// The asset ID a source file cooks to (its main asset), from its meta's GUID: content_path is "src:/content/a/b.world",
// its meta "src:/meta/a/b.world.json". False if it has no meta, or the meta has no GUID.
bool source_asset_id(const platform::Vfs& vfs, std::string_view content_path, asset::Guid& guid, asset::AssetId& id);

} // namespace de::editor
