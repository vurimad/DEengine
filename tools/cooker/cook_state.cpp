#include "cooker/cook_state.h"

#include "platform/vfs.h"

#include <utility>

namespace de::cooker {

namespace {

constexpr int kStateVersion = 1;

// Hashes are written like asset IDs: 16 hex digits (JSON numbers lose bits above 2^53). 0 is a
// valid value here: a failed source's input hash, which never matches, so it cooks again.
bool parse_hash(yyjson_val* value, uint64_t& out) {
    return yyjson_is_str(value) && asset::parse_hex64({yyjson_get_str(value), yyjson_get_len(value)}, out);
}

void add_hash(yyjson_mut_doc* doc, yyjson_mut_val* object, const char* key, uint64_t hash) {
    const asset::AssetIdText text = asset::format_asset_id(asset::AssetId::from_value(hash));
    yyjson_mut_obj_add_strncpy(doc, object, key, text.c_str(), text.size());
}

asset::AssetType parse_type(std::string_view name) {
    for (const asset::AssetType type : {asset::AssetType::Mesh, asset::AssetType::Texture, asset::AssetType::Material,
                                        asset::AssetType::Shader, asset::AssetType::Cell}) {
        if (name == asset::asset_type_name(type)) {
            return type;
        }
    }
    return asset::AssetType::Unknown;
}

std::string_view string_of(yyjson_val* value) {
    return yyjson_is_str(value) ? std::string_view(yyjson_get_str(value), yyjson_get_len(value)) : std::string_view();
}

bool read_record(yyjson_val* entry, CookRecord& record) {
    if (!record.path.assign(string_of(yyjson_obj_get(entry, "path"))) || record.path.empty() ||
        !parse_hash(yyjson_obj_get(entry, "source_hash"), record.source_hash) ||
        !parse_hash(yyjson_obj_get(entry, "input_hash"), record.input_hash)) {
        return false;
    }
    if (yyjson_val* guid = yyjson_obj_get(entry, "guid");
        guid != nullptr && !asset::parse_guid(string_of(guid), record.guid)) {
        return false;
    }
    size_t index = 0;
    size_t count = 0;
    yyjson_val* item = nullptr;
    yyjson_arr_foreach(yyjson_obj_get(entry, "outputs"), index, count, item) {
        CookedOutput output{.type = parse_type(string_of(yyjson_obj_get(item, "type")))};
        if (!asset::parse_asset_id(string_of(yyjson_obj_get(item, "id")), output.id) ||
            output.type == asset::AssetType::Unknown) {
            return false;
        }
        record.outputs.push(output);
    }
    yyjson_arr_foreach(yyjson_obj_get(entry, "dependencies"), index, count, item) {
        SourcePath& path = record.dependencies.emplace();
        if (!path.assign(string_of(item)) || path.empty()) {
            return false;
        }
    }
    yyjson_arr_foreach(yyjson_obj_get(entry, "texture_uses"), index, count, item) {
        TextureUse& use = record.texture_uses.emplace();
        if (!use.path.assign(string_of(yyjson_obj_get(item, "path"))) || use.path.empty() ||
            !parse_texture_usage(string_of(yyjson_obj_get(item, "usage")), use.usage)) {
            return false;
        }
    }
    return true;
}

} // namespace

CookState CookState::load(const platform::Vfs& vfs, Diagnostics& diagnostics) {
    CookState state;
    if (!vfs.exists(kPath)) {
        return state;
    }
    const core::Result<core::Array<std::byte>> text = read_whole_file(vfs, kPath);
    if (!text) {
        diagnostics.warning("", "%s could not be read: cooking everything", kPath);
        return state;
    }
    Diagnostics ignored(false); // a broken state is reported once, below, as a warning
    const core::Result<JsonDoc> doc = JsonDoc::parse(text->span(), kPath, ignored);
    yyjson_val* root = doc ? doc->root() : nullptr;
    if (root == nullptr || yyjson_get_int(yyjson_obj_get(root, "version")) != kStateVersion) {
        diagnostics.warning("", "%s is broken or from another cooker version: cooking everything", kPath);
        return state;
    }
    size_t index = 0;
    size_t count = 0;
    yyjson_val* entry = nullptr;
    yyjson_arr_foreach(yyjson_obj_get(root, "sources"), index, count, entry) {
        CookRecord record;
        if (!read_record(entry, record)) {
            diagnostics.warning("", "%s is broken: cooking everything", kPath);
            return {};
        }
        state.put(std::move(record));
    }
    return state;
}

core::Result<void> CookState::save(const platform::Vfs& vfs) const {
    const JsonBuilder json;
    yyjson_mut_doc* doc = json.doc();
    yyjson_mut_val* root = json.root();
    yyjson_mut_obj_add_int(doc, root, "version", kStateVersion);
    yyjson_mut_val* sources = yyjson_mut_obj_add_arr(doc, root, "sources");
    for (const CookRecord& record : records_) {
        yyjson_mut_val* entry = yyjson_mut_arr_add_obj(doc, sources);
        yyjson_mut_obj_add_strncpy(doc, entry, "path", record.path.c_str(), record.path.size());
        if (record.guid.is_valid()) {
            const asset::GuidText guid = asset::format_guid(record.guid);
            yyjson_mut_obj_add_strncpy(doc, entry, "guid", guid.c_str(), guid.size());
        }
        add_hash(doc, entry, "source_hash", record.source_hash);
        add_hash(doc, entry, "input_hash", record.input_hash);
        yyjson_mut_val* outputs = yyjson_mut_obj_add_arr(doc, entry, "outputs");
        for (const CookedOutput& output : record.outputs) {
            yyjson_mut_val* item = yyjson_mut_arr_add_obj(doc, outputs);
            const asset::AssetIdText id = asset::format_asset_id(output.id);
            yyjson_mut_obj_add_strncpy(doc, item, "id", id.c_str(), id.size());
            yyjson_mut_obj_add_str(doc, item, "type", asset::asset_type_name(output.type));
        }
        if (!record.dependencies.empty()) {
            yyjson_mut_val* dependencies = yyjson_mut_obj_add_arr(doc, entry, "dependencies");
            for (const SourcePath& path : record.dependencies) {
                yyjson_mut_arr_add_strncpy(doc, dependencies, path.c_str(), path.size());
            }
        }
        if (!record.texture_uses.empty()) {
            yyjson_mut_val* uses = yyjson_mut_obj_add_arr(doc, entry, "texture_uses");
            for (const TextureUse& use : record.texture_uses) {
                yyjson_mut_val* item = yyjson_mut_arr_add_obj(doc, uses);
                yyjson_mut_obj_add_strncpy(doc, item, "path", use.path.c_str(), use.path.size());
                yyjson_mut_obj_add_str(doc, item, "usage", texture_usage_name(use.usage));
            }
        }
    }
    const core::Array<char> text = json.write(true);
    if (text.empty()) {
        return core::fail(core::ErrorCode::OutOfMemory, "the cook state could not be written");
    }
    return vfs.write_file(kPath, std::as_bytes(text.span()));
}

const CookRecord* CookState::find(std::string_view path) const {
    const uint32_t* index = by_path_.find(path_key(path));
    return index != nullptr && records_[*index].path == path ? &records_[*index] : nullptr;
}

const CookRecord* CookState::find(asset::Guid guid) const {
    for (const CookRecord& record : records_) {
        if (record.guid == guid) {
            return &record;
        }
    }
    return nullptr;
}

void CookState::put(CookRecord record) {
    if (const uint32_t* index = by_path_.find(path_key(record.path.view())); index != nullptr) {
        if (records_[*index].path == record.path.view()) {
            records_[*index] = std::move(record);
            return;
        }
    }
    by_path_.set(path_key(record.path.view()), static_cast<uint32_t>(records_.size()));
    records_.push(std::move(record));
}

void CookState::remove(std::string_view path) {
    for (size_t i = 0; i < records_.size(); ++i) {
        if (records_[i].path == path) {
            records_.remove_ordered(i);
            reindex();
            return;
        }
    }
}

void CookState::reindex() {
    by_path_.clear();
    for (size_t i = 0; i < records_.size(); ++i) {
        by_path_.set(path_key(records_[i].path.view()), static_cast<uint32_t>(i));
    }
}

} // namespace de::cooker
