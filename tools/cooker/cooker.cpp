#include "cooker/cooker.h"

#include "asset/cell_format.h"
#include "asset/environment_format.h"
#include "asset/material_format.h"
#include "asset/mesh_format.h"
#include "asset/shader_format.h"
#include "asset/texture_format.h"
#include "asset/world_format.h"
#include "cooker/cell_cooker.h"
#include "cooker/cook_state.h"
#include "cooker/material_cooker.h"
#include "cooker/mesh_cooker.h"
#include "cooker/model.h"
#include "cooker/shader_cooker.h"
#include "cooker/source_tree.h"
#include "cooker/texture_cooker.h"
#include "cooker/third_party.h"
#include "cooker/world_cooker.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/profile.h"
#include "platform/file.h"
#include "platform/vfs.h"
#include "render/shader/shader_asset.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace de::cooker {

namespace {

// Bumped when the cooker writes different files for the same inputs (a better encoder, a fixed
// bug): every source cooks again. Format versions are hashed in too, per kind.
constexpr uint64_t kCookerVersion = 6; // 2: cell children are as static as their parent; 6: longer LOD chains

constexpr std::string_view kShaderTests = "shaders/tests/";

// Chained content hashes.
class Hasher {
public:
    explicit Hasher(std::string_view kind) { add(kind); }
    void add(std::span<const std::byte> bytes) { value_ = core::hash_bytes(bytes.data(), bytes.size(), value_); }
    void add(std::string_view text) { add(std::as_bytes(std::span(text.data(), text.size()))); }
    void add(uint64_t number) { add(std::as_bytes(std::span(&number, 1))); }
    uint64_t value() const { return value_ != 0 ? value_ : 1; } // 0 is "never matches"

private:
    uint64_t value_ = kCookerVersion;
};

CookRecord clone(const CookRecord& record) {
    CookRecord copy{
        .path = record.path, .guid = record.guid, .source_hash = record.source_hash, .input_hash = record.input_hash};
    copy.outputs.assign(record.outputs.span());
    copy.dependencies.assign(record.dependencies.span());
    copy.texture_uses.assign(record.texture_uses.span());
    return copy;
}

class Cook {
public:
    Cook(platform::Vfs& vfs, core::JobSystem& jobs, shader::ShaderCompiler* compiler, const CookOptions& options)
        : vfs_(vfs), jobs_(jobs), compiler_(compiler), options_(options) {}

    CookReport run() {
        previous_ = CookState::load(vfs_, diagnostics_);
        ScanStats scan;
        if (options_.content) {
            tree_ = SourceTree::scan(vfs_, previous_, {.write_metas = !options_.check_only}, diagnostics_, &scan);
        }
        stats_.metas_created = scan.metas_created;
        stats_.metas_relinked = scan.metas_relinked;
        if (!options_.check_only) {
            cook_all();
        } else if (options_.check_shaders) {
            check_cooked_shaders();
        }
        CookReport report{.stats = stats_};
        report.diagnostics = diagnostics_.all();
        DE_LOG_INFO("cooker",
                    "%s: %u cooked, %u up to date, %u failed, %u removed; metas: %u created, %u re-linked; %zu errors, "
                    "%zu warnings",
                    options_.check_only ? "check" : "cook", stats_.cooked, stats_.up_to_date, stats_.failed,
                    stats_.removed, stats_.metas_created, stats_.metas_relinked, diagnostics_.error_count(),
                    diagnostics_.warning_count());
        return report;
    }

private:
    void cook_all() {
        // Sources the scan left out (broken meta, a GUID another file has): failed, so what was
        // cooked from them stays until they're sorted out.
        for (const SourcePath& path : tree_.skipped()) {
            failed(path.view());
        }
        cook_shaders();
        copy_config();
        if (!options_.content) {
            keep_content();
        }
        // Models and materials first: how they use textures picks the textures' defaults.
        for (const Source& source : tree_.sources()) {
            if (source.kind == SourceKind::Gltf || source.kind == SourceKind::Fbx) {
                cook_model(source);
            } else if (source.kind == SourceKind::Material) {
                cook_text_material(source);
            } else if (source.kind == SourceKind::Cell) {
                cook_cell_source(source);
            } else if (source.kind == SourceKind::World) {
                cook_world_source(source);
            } else if (source.kind == SourceKind::Environment) {
                cook_environment_source(source);
            }
        }
        for (const Source& source : tree_.sources()) {
            if (source.kind == SourceKind::Texture) {
                cook_texture_source(source);
            }
        }
        remove_stale();
        if (const core::Result<void> saved = next_.save(vfs_); !saved) {
            diagnostics_.error("", "%s could not be written: %s", CookState::kPath, saved.error().message);
        }
    }

    // Not cooking content this time: what was cooked from it stays, untouched.
    void keep_content() {
        for (const CookRecord& record : previous_.records()) {
            if (!record.path.view().starts_with("shaders/")) {
                for (const CookedOutput& output : record.outputs) {
                    static_cast<void>(claim(record.path.view(), output.id));
                }
                next_.put(clone(record));
            }
        }
    }

    // A source whose inputs hash as they did last time, with its cooked files still there.
    const CookRecord* unchanged(std::string_view path, uint64_t input_hash) const {
        const CookRecord* record = previous_.find(path);
        if (options_.force || record == nullptr || record->input_hash != input_hash) {
            return nullptr;
        }
        for (const CookedOutput& output : record->outputs) {
            if (!vfs_.exists(asset::cooked_path(output.id, output.type).c_str())) {
                return nullptr;
            }
        }
        return record;
    }

    void keep(const CookRecord& record) {
        for (const TextureUse& use : record.texture_uses) {
            note_use(use, record.path.view());
        }
        for (const CookedOutput& output : record.outputs) {
            static_cast<void>(claim(record.path.view(), output.id));
        }
        next_.put(clone(record));
        ++stats_.up_to_date;
    }

    // One source per asset ID: claims it for path, or reports a collision (two GUIDs, or two
    // names, deriving the same ID) with whichever source claimed it first this run.
    bool claim(std::string_view path, asset::AssetId id) {
        const uint64_t key = path_key(path);
        if (const uint64_t* owner = written_.find(id.value()); owner != nullptr && *owner != key) {
            diagnostics_.error(path, "cooks to asset %s, which another source makes too (a GUID or name collision)",
                               asset::format_asset_id(id).c_str());
            return false;
        }
        written_.set(id.value(), key);
        return true;
    }

    // A failed source keeps its last cooked files and record (so they aren't removed as stale),
    // with an input hash that never matches: it cooks again next time. Files this attempt wrote
    // before failing (partial) join the record, so they are tracked and removed once stale too.
    // Its texture uses carry over too (the last good ones, else this attempt's), so a texture
    // doesn't lose its defaults because the model using it broke.
    void failed(std::string_view path, const CookRecord* partial = nullptr) {
        ++stats_.failed;
        const CookRecord* record = previous_.find(path);
        const CookRecord* uses = record != nullptr ? record : partial;
        if (uses != nullptr) {
            for (const TextureUse& use : uses->texture_uses) {
                note_use(use, path);
            }
        }
        if (record != nullptr) {
            for (const CookedOutput& output : record->outputs) {
                static_cast<void>(claim(path, output.id));
            }
        }
        if (record == nullptr && (partial == nullptr || partial->outputs.empty())) {
            return;
        }
        CookRecord kept = record != nullptr ? clone(*record) : CookRecord{.path = partial->path, .guid = partial->guid};
        kept.input_hash = 0;
        if (partial != nullptr) {
            for (const CookedOutput& output : partial->outputs) {
                const bool known =
                    std::ranges::any_of(kept.outputs, [&](const CookedOutput& other) { return other.id == output.id; });
                if (!known) {
                    kept.outputs.push(output);
                }
            }
        }
        next_.put(std::move(kept));
    }

    void note_use(const TextureUse& use, std::string_view user) {
        TextureUsage& usage = usages_.find_or_insert(path_key(use.path.view()));
        if (usage == TextureUsage::Unknown) {
            usage = use.usage;
        } else if (use.usage != TextureUsage::Unknown && use.usage != usage) {
            diagnostics_.warning(use.path.view(), "is used as %s by %.*s and as %s elsewhere: cooked for %s use",
                                 texture_usage_name(use.usage), static_cast<int>(user.size()), user.data(),
                                 texture_usage_name(usage), texture_usage_name(usage));
        }
    }

    bool write(CookRecord& record, asset::AssetId id, asset::AssetType type, const core::Array<std::byte>& bytes) {
        if (std::ranges::any_of(record.outputs, [&](const CookedOutput& output) { return output.id == id; })) {
            diagnostics_.error(record.path.view(), "makes asset %s twice (two sub-assets with one name)",
                               asset::format_asset_id(id).c_str());
            return false;
        }
        if (!claim(record.path.view(), id)) {
            return false;
        }
        const asset::CookedPath path = asset::cooked_path(id, type);
        if (const core::Result<void> written = vfs_.write_file(path.c_str(), bytes.span()); !written) {
            diagnostics_.error(record.path.view(), "%s could not be written: %s", path.c_str(),
                               written.error().message);
            return false;
        }
        record.outputs.push(CookedOutput{.id = id, .type = type});
        return true;
    }

    core::Result<core::Array<std::byte>> read_source(std::string_view path) {
        core::Result<core::Array<std::byte>> bytes = read_whole_file(vfs_, src_path(path).c_str());
        if (!bytes) {
            diagnostics_.error(path, "could not be read: %s", bytes.error().message);
        }
        return bytes;
    }

    // The IDs of the textures a model or material references (a renamed or re-minted texture
    // changes the material that points at it); a missing one hashes as missing.
    void hash_texture_ids(Hasher& hasher, std::span<const TextureUse> uses) const {
        for (const TextureUse& use : uses) {
            hash_source_id(hasher, use.path.view());
        }
    }

    // A source's path and the ID it has now (0 if it's gone).
    void hash_source_id(Hasher& hasher, std::string_view path) const {
        hasher.add(path);
        const Source* source = tree_.find(path);
        hasher.add(source != nullptr ? source->id.value() : 0);
    }

    // The IDs of other sources an asset names (a cell's models): a re-minted one changes it. A
    // missing one hashes as missing.
    void hash_source_ids(Hasher& hasher, std::span<const SourcePath> sources) const {
        for (const SourcePath& path : sources) {
            hash_source_id(hasher, path.view());
        }
    }

    // Each file's path and bytes (a missing file hashes as missing).
    void hash_files(Hasher& hasher, std::span<const SourcePath> files) {
        for (const SourcePath& file : files) {
            hasher.add(file.view());
            hasher.add(file_hash(file.view()));
        }
    }

    // A file's content hash, read once per run however many sources depend on it (shader imports);
    // 0 for a missing file.
    uint64_t file_hash(std::string_view path) {
        if (const uint64_t* known = file_hashes_.find(path_key(path)); known != nullptr) {
            return *known;
        }
        const core::Result<core::Array<std::byte>> bytes = read_whole_file(vfs_, src_path(path).c_str());
        const uint64_t hash = bytes ? core::hash_bytes(bytes->data(), bytes->size()) : 0;
        file_hashes_.set(path_key(path), hash);
        return hash;
    }

    // --- Config ------------------------------------------------------------------------------

    // config/ (input bindings, engine settings: text the engine reads as it is) into game:/config/,
    // where ship builds find it. Copied when its bytes differ.
    void copy_config() {
        core::Array<SourcePath> files(tools_heap());
        list_tree(vfs_, "config", true, diagnostics_, files);
        for (const SourcePath& file : files) {
            const core::Result<core::Array<std::byte>> bytes = read_source(file.view());
            if (!bytes) {
                continue;
            }
            core::FixedString<kMaxSourcePath + 8> target("game:/");
            static_cast<void>(target.append(file.view()));
            const core::Result<core::Array<std::byte>> current = read_whole_file(vfs_, target.c_str());
            if (current && current->size() == bytes->size() &&
                std::memcmp(current->data(), bytes->data(), bytes->size()) == 0) {
                continue;
            }
            if (const core::Result<void> written = vfs_.write_file(target.c_str(), bytes->span()); !written) {
                diagnostics_.error(file.view(), "%s could not be written: %s", target.c_str(), written.error().message);
            }
        }
    }

    // --- Shaders -----------------------------------------------------------------------------

    // Not cooking shaders this time: what was cooked stays, untouched.
    void keep_cooked_shaders() {
        for (const CookRecord& record : previous_.records()) {
            if (record.path.view().starts_with("shaders/")) {
                for (const CookedOutput& output : record.outputs) {
                    static_cast<void>(claim(record.path.view(), output.id));
                }
                next_.put(clone(record));
            }
        }
    }

    void cook_shaders() {
        if (compiler_ == nullptr) {
            keep_cooked_shaders(); // --no-shaders
            return;
        }
        // The stamp says every cooked shader comes from these sources, so it goes before the first
        // is written and comes back only if they all made it: a cook that fails or stops halfway
        // leaves a mix of old and new, which no engine should load. If it can't go, nothing's
        // written: it stays true of what's cooked. Without one, the last cook didn't finish, and
        // its records can't be trusted (a file saved during a compile can be hashed as compiled),
        // so every module cooks.
        const bool last_cook_finished = vfs_.exists(asset::kShaderStampPath);
        if (const core::Result<void> removed = vfs_.remove_file(asset::kShaderStampPath); !removed) {
            diagnostics_.error("", "%s could not be removed, so no shaders were cooked: %s", asset::kShaderStampPath,
                               removed.error().message);
            ++stats_.failed;
            keep_cooked_shaders();
            return;
        }
        const core::Result<uint64_t> sources = asset::shader_sources_hash(vfs_);
        if (!sources && vfs_.exists("src:/shaders")) {
            diagnostics_.error("shaders", "could not be hashed: %s", sources.error().message);
        }
        core::Array<SourcePath> files(tools_heap());
        const size_t errors_before_listing = diagnostics_.error_count();
        list_tree(vfs_, "shaders", false, diagnostics_, files);
        bool all_cooked = diagnostics_.error_count() == errors_before_listing; // a path too long is left out
        const asset::ShaderFlags flags =
            (options_.shader_debug_info ? asset::ShaderFlags::DebugInfo : asset::ShaderFlags::None) |
            (options_.shader_checks ? asset::ShaderFlags::Checks : asset::ShaderFlags::None);
        for (const SourcePath& file : files) {
            shader::ShaderName module;
            const bool test_module = file.view().starts_with(kShaderTests); // games never load these
            if (!options_.test_shaders && test_module) {
                continue;
            }
            if (!shader::shader_module_name(file.view(), module)) {
                if (extension_of(file.view()) == ".slang") {
                    diagnostics_.error(file.view(), "its module name is longer than %zu characters: it can't be cooked",
                                       shader::kMaxShaderNameLength);
                    all_cooked = all_cooked && test_module;
                }
                continue;
            }
            const core::Result<core::Array<std::byte>> bytes = read_source(file.view());
            if (!bytes) {
                failed(file.view()); // keeps what was cooked from it
                all_cooked = all_cooked && test_module;
                continue;
            }
            if (!defines_entry_points(bytes->span())) {
                continue;
            }
            const uint64_t source_hash = core::hash_bytes(bytes->data(), bytes->size());
            const CookRecord* last = previous_.find(file.view());
            const auto input_hash = [&](std::span<const SourcePath> dependencies) {
                Hasher hasher("shader");
                hasher.add(asset::ShaderAsset::kFormatVersion);
                hasher.add(static_cast<uint64_t>(flags));
                hasher.add(source_hash);
                hash_files(hasher, dependencies);
                return hasher.value();
            };
            if (last != nullptr && last_cook_finished) {
                if (const CookRecord* same = unchanged(file.view(), input_hash(last->dependencies.span()));
                    same != nullptr) {
                    keep(*same);
                    continue;
                }
            }
            CookRecord record{.path = file, .source_hash = source_hash};
            core::Result<core::Array<std::byte>> cooked =
                cook_shader(*compiler_, module.view(), flags, record.dependencies, file.view(), diagnostics_);
            if (!cooked || !write(record, asset::shader_asset_id(module.view()), asset::AssetType::Shader, *cooked)) {
                failed(file.view());
                all_cooked = all_cooked && test_module;
                continue;
            }
            std::ranges::sort(record.dependencies,
                              [](const SourcePath& a, const SourcePath& b) { return a.view() < b.view(); });
            record.input_hash = input_hash(record.dependencies.span());
            next_.put(std::move(record));
            ++stats_.cooked;
        }
        if (!sources || !all_cooked) {
            return;
        }
        if (options_.before_shader_stamp != nullptr) {
            options_.before_shader_stamp(options_.before_shader_stamp_context);
        }
        // Saved during the cook: what's cooked may be from either side of it.
        if (const core::Result<uint64_t> after = asset::shader_sources_hash(vfs_); !after || *after != *sources) {
            diagnostics_.error("shaders", "changed during the cook, so they aren't stamped: re-run the cooker");
            ++stats_.failed;
            return;
        }
        if (const core::Result<void> stamped = asset::write_shader_stamp(vfs_, *sources); !stamped) {
            diagnostics_.error("", "%s could not be written: %s", asset::kShaderStampPath, stamped.error().message);
        }
    }

    // Whether game:/ holds any cooked shader: its files are the answer, not the state a killed cook
    // never saved. They lie one folder down (asset::cooked_path).
    bool any_cooked_shader() const {
        const std::string_view extension = asset::asset_type_extension(asset::AssetType::Shader);
        bool found = false;
        static_cast<void>(vfs_.list("game:/", [&](const char* folder, bool folder_is_directory) {
            platform::PathString path("game:/");
            if (found || !folder_is_directory || !path.append(folder)) {
                return;
            }
            static_cast<void>(vfs_.list(path.c_str(), [&](const char* name, bool is_directory) {
                found = found || (!is_directory && std::string_view(name).ends_with(extension));
            }));
        }));
        return found;
    }

    // --check: the stamp against the sources as they are now (not this cooker's own build, which
    // may be older). No cooked shaders at all (CI) is nothing stale; cooked ones without a stamp are.
    void check_cooked_shaders() {
        const core::Result<uint64_t> sources = asset::shader_sources_hash(vfs_);
        if (!sources) {
            if (vfs_.exists("src:/shaders")) {
                diagnostics_.error("shaders", "could not be hashed: %s", sources.error().message);
            }
            return;
        }
        const core::Result<uint64_t> stamp = asset::read_shader_stamp(vfs_);
        if (!stamp && stamp.error().code == core::ErrorCode::NotFound) {
            if (any_cooked_shader()) {
                diagnostics_.error("shaders", "the cooked shaders are stale: the last cook failed or stopped, or an "
                                              "older cooker made them: re-run the cooker");
            } else {
                DE_LOG_INFO("cooker", "no cooked shaders to check");
            }
        } else if (!stamp) {
            diagnostics_.error("shaders", "%s: %s: re-run the cooker", asset::kShaderStampPath, stamp.error().message);
        } else if (*stamp != *sources) {
            diagnostics_.error("shaders",
                               "the cooked shaders are stale: cooked from sources %016llx, the sources are now "
                               "%016llx: re-run the cooker",
                               static_cast<unsigned long long>(*stamp), static_cast<unsigned long long>(*sources));
        }
    }

    // --- Models ------------------------------------------------------------------------------

    void cook_model(const Source& source) {
        const std::string_view path = source.path.view();
        const core::Result<core::Array<std::byte>> bytes = read_source(path);
        if (!bytes) {
            failed(path);
            return;
        }
        JsonBuilder mesh_json;
        tree_.resolve_settings(source, "mesh", mesh_json);
        // For its embedded images, over their usage defaults: the meta's own section only, since
        // folder rules match file names, and this is the model's ("gas_mask.glb" isn't a mask).
        JsonBuilder texture_json;
        tree_.resolve_settings(source, "texture", texture_json, false);
        const uint64_t source_hash = core::hash_bytes(bytes->data(), bytes->size());
        const auto input_hash = [&](std::span<const SourcePath> dependencies, std::span<const TextureUse> uses) {
            Hasher hasher(source_kind_name(source.kind));
            hasher.add(source.id.value()); // a new GUID means new IDs for everything it makes
            hasher.add(asset::MeshAsset::kFormatVersion);
            hasher.add(asset::MaterialAsset::kFormatVersion);
            hasher.add(asset::TextureAsset::kFormatVersion);
            hasher.add(source_hash);
            hasher.add(std::as_bytes(mesh_json.write(false).span()));
            hasher.add(std::as_bytes(texture_json.write(false).span()));
            hash_texture_ids(hasher, uses);
            hash_files(hasher, dependencies);
            return hasher.value();
        };
        if (const CookRecord* last = previous_.find(path); last != nullptr) {
            if (const CookRecord* same =
                    unchanged(path, input_hash(last->dependencies.span(), last->texture_uses.span()));
                same != nullptr) {
                keep(*same);
                return;
            }
        }

        const size_t errors_before = diagnostics_.error_count();
        const MeshSettings mesh_settings = read_mesh_settings(mesh_json.freeze().root(), path, diagnostics_);
        core::Result<ModelData> imported = source.kind == SourceKind::Gltf
                                               ? import_gltf(vfs_, tree_, source, bytes->span(), diagnostics_)
                                               : import_fbx(tree_, source, bytes->span(), diagnostics_);
        if (!imported || diagnostics_.error_count() != errors_before) {
            failed(path);
            return;
        }
        const ModelData& model = *imported;
        CookRecord record{.path = source.path, .guid = source.guid, .source_hash = source_hash};
        record.dependencies.assign(model.dependencies.span());

        // Textures: sources of their own, or embedded images cooked here as sub-assets.
        core::Array<asset::AssetId> texture_ids(tools_heap());
        bool ok = true;
        for (const ModelTexture& texture : model.textures) {
            if (!texture.file.empty()) {
                texture_ids.push(tree_.find(texture.file.view())->id);
                record.texture_uses.push(TextureUse{.path = texture.file, .usage = texture.usage});
                continue;
            }
            core::FixedString<32> name("image/");
            static_cast<void>(name.append(number_text(model.images[texture.embedded].key).view()));
            const asset::AssetId id = asset::sub_asset_id(source.id, name.view());
            texture_ids.push(id);
            ok = ok && cook_embedded_image(record, model.images[texture.embedded], texture.usage, texture_json, id);
        }

        // Materials: named sub-assets ("material/stone"), by index when a name is missing or repeated.
        core::Array<asset::AssetId> material_ids(tools_heap());
        for (size_t i = 0; i < model.materials.size(); ++i) {
            const ModelMaterial& material = model.materials[i];
            // "#<index>" is the form for unnamed and repeated names, so names can't take it.
            bool unique = !material.name.empty() && !material.name.view().starts_with('#');
            for (size_t j = 0; j < model.materials.size() && unique; ++j) {
                unique = j == i || model.materials[j].name != material.name.view();
            }
            core::FixedString<kMaxMaterialName + 16> name("material/");
            const bool fits =
                unique ? name.append(material.name.view()) : name.append('#') && name.append(number_text(i).view());
            DE_ASSERT(fits);
            static_cast<void>(fits);
            const asset::AssetId id = asset::sub_asset_id(source.id, name.view());
            material_ids.push(id);
            MaterialBuild build = material_from_model(material, texture_ids.span());
            ok = ok && write(record, id, asset::AssetType::Material, cook_material(build, id));
        }

        core::Result<core::Array<std::byte>> mesh =
            ok ? cook_mesh(model, material_ids.span(), mesh_settings, source.id, path, diagnostics_)
               : core::Result<core::Array<std::byte>>(core::fail(core::ErrorCode::Unknown, ""));
        if (mesh && diagnostics_.error_count() != errors_before) {
            mesh = core::fail(core::ErrorCode::InvalidArgument, "the model has errors"); // reported above
        }
        if (!mesh || !write(record, source.id, asset::AssetType::Mesh, *mesh)) {
            failed(path, &record);
            return;
        }
        record.input_hash = input_hash(record.dependencies.span(), record.texture_uses.span());
        for (const TextureUse& use : record.texture_uses) {
            note_use(use, path);
        }
        next_.put(std::move(record));
        ++stats_.cooked;
    }

    bool cook_embedded_image(CookRecord& record, const ModelImage& image, TextureUsage usage,
                             const JsonBuilder& overrides, asset::AssetId id) {
        const std::string_view path = record.path.view();
        JsonBuilder settings_json;
        texture_usage_defaults(usage, settings_json);
        settings_json.merge(overrides.freeze().root());
        const size_t errors_before = diagnostics_.error_count();
        const TextureSettings settings = read_texture_settings(settings_json.freeze().root(), path, diagnostics_);
        if (diagnostics_.error_count() != errors_before) {
            return false;
        }
        const core::Result<Image> decoded = decode_image(image.encoded.span(), path, diagnostics_);
        if (!decoded) {
            return false;
        }
        const core::Result<core::Array<std::byte>> cooked =
            cook_texture(*decoded, settings, id, &jobs_, path, diagnostics_);
        return cooked && write(record, id, asset::AssetType::Texture, *cooked);
    }

    // --- Text materials ----------------------------------------------------------------------

    void cook_text_material(const Source& source) {
        const std::string_view path = source.path.view();
        const core::Result<core::Array<std::byte>> bytes = read_source(path);
        if (!bytes) {
            failed(path);
            return;
        }
        const uint64_t source_hash = core::hash_bytes(bytes->data(), bytes->size());
        const auto input_hash = [&](std::span<const TextureUse> uses) {
            Hasher hasher("material");
            hasher.add(source.id.value());
            hasher.add(asset::MaterialAsset::kFormatVersion);
            hasher.add(source_hash);
            hash_texture_ids(hasher, uses);
            return hasher.value();
        };
        if (const CookRecord* last = previous_.find(path); last != nullptr) {
            if (const CookRecord* same = unchanged(path, input_hash(last->texture_uses.span())); same != nullptr) {
                keep(*same);
                return;
            }
        }
        CookRecord record{.path = source.path, .guid = source.guid, .source_hash = source_hash};
        core::Result<MaterialBuild> build =
            import_text_material(tree_, source, bytes->span(), record.texture_uses, diagnostics_);
        if (!build || !write(record, source.id, asset::AssetType::Material, cook_material(*build, source.id))) {
            failed(path, &record);
            return;
        }
        record.input_hash = input_hash(record.texture_uses.span());
        for (const TextureUse& use : record.texture_uses) {
            note_use(use, path);
        }
        next_.put(std::move(record));
        ++stats_.cooked;
    }

    // --- Cells -------------------------------------------------------------------------------

    // The models a cell places are its record's dependencies, hashed by ID (not bytes: a model
    // cooked again doesn't change the cell).
    void cook_cell_source(const Source& source) {
        const std::string_view path = source.path.view();
        const core::Result<core::Array<std::byte>> bytes = read_source(path);
        if (!bytes) {
            failed(path);
            return;
        }
        const uint64_t source_hash = core::hash_bytes(bytes->data(), bytes->size());
        const auto input_hash = [&](std::span<const SourcePath> meshes) {
            Hasher hasher("cell");
            hasher.add(source.id.value());
            hasher.add(asset::CellAsset::kFormatVersion);
            hasher.add(source_hash);
            hash_source_ids(hasher, meshes);
            return hasher.value();
        };
        if (const CookRecord* last = previous_.find(path); last != nullptr) {
            if (const CookRecord* same = unchanged(path, input_hash(last->dependencies.span())); same != nullptr) {
                keep(*same);
                return;
            }
        }
        CookRecord record{.path = source.path, .guid = source.guid, .source_hash = source_hash};
        const core::Result<CellBuild> build = import_cell(tree_, source, bytes->span(), diagnostics_);
        if (!build || !write(record, source.id, asset::AssetType::Cell, cook_cell(*build, source.id))) {
            failed(path, &record);
            return;
        }
        record.dependencies.assign(build->mesh_sources.span());
        record.input_hash = input_hash(record.dependencies.span());
        next_.put(std::move(record));
        ++stats_.cooked;
    }

    // --- Worlds and environments --------------------------------------------------------------

    // A world reads its cells' coords: their bytes are its inputs, with their IDs and its
    // environment's (a re-minted one changes it). The environment's bytes aren't: a world names it,
    // so editing the light doesn't cook the world again.
    void cook_world_source(const Source& source) {
        const std::string_view path = source.path.view();
        const core::Result<core::Array<std::byte>> bytes = read_source(path);
        if (!bytes) {
            failed(path);
            return;
        }
        const uint64_t source_hash = core::hash_bytes(bytes->data(), bytes->size());
        const auto input_hash = [&](std::span<const SourcePath> read) {
            Hasher hasher("world");
            hasher.add(source.id.value());
            hasher.add(asset::WorldAsset::kFormatVersion);
            hasher.add(source_hash);
            for (const SourcePath& file : read) {
                if (extension_of(file.view()) == ".cell") {
                    hash_files(hasher, std::span(&file, 1));
                }
            }
            hash_source_ids(hasher, read);
            return hasher.value();
        };
        if (const CookRecord* last = previous_.find(path); last != nullptr) {
            if (const CookRecord* same = unchanged(path, input_hash(last->dependencies.span())); same != nullptr) {
                keep(*same);
                return;
            }
        }
        CookRecord record{.path = source.path, .guid = source.guid, .source_hash = source_hash};
        const auto read = [this](std::string_view source_path) { return read_source(source_path); };
        const core::Result<WorldBuild> build = import_world(tree_, source, bytes->span(), read, diagnostics_);
        if (!build || !write(record, source.id, asset::AssetType::World, cook_world(*build, source.id))) {
            failed(path, &record);
            return;
        }
        record.dependencies.assign(build->read.span());
        record.input_hash = input_hash(record.dependencies.span());
        next_.put(std::move(record));
        ++stats_.cooked;
    }

    void cook_environment_source(const Source& source) {
        const std::string_view path = source.path.view();
        const core::Result<core::Array<std::byte>> bytes = read_source(path);
        if (!bytes) {
            failed(path);
            return;
        }
        const uint64_t source_hash = core::hash_bytes(bytes->data(), bytes->size());
        Hasher hasher("environment");
        hasher.add(source.id.value());
        hasher.add(asset::EnvironmentAsset::kFormatVersion);
        hasher.add(source_hash);
        if (const CookRecord* same = unchanged(path, hasher.value()); same != nullptr) {
            keep(*same);
            return;
        }
        CookRecord record{.path = source.path, .guid = source.guid, .source_hash = source_hash};
        const core::Result<asset::EnvironmentAsset> environment =
            import_environment(source, bytes->span(), diagnostics_);
        if (!environment ||
            !write(record, source.id, asset::AssetType::Environment, cook_environment(*environment, source.id))) {
            failed(path, &record);
            return;
        }
        record.input_hash = hasher.value();
        next_.put(std::move(record));
        ++stats_.cooked;
    }

    // --- Textures ----------------------------------------------------------------------------

    void cook_texture_source(const Source& source) {
        const std::string_view path = source.path.view();
        const TextureUsage* used = usages_.find(path_key(path));
        JsonBuilder settings_json;
        texture_usage_defaults(used != nullptr ? *used : TextureUsage::Unknown, settings_json);
        tree_.resolve_settings(source, "texture", settings_json);
        const size_t errors_before = diagnostics_.error_count();
        const TextureSettings settings = read_texture_settings(settings_json.freeze().root(), path, diagnostics_);
        const core::Result<core::Array<std::byte>> bytes = read_source(path);
        if (!bytes || diagnostics_.error_count() != errors_before) {
            failed(path);
            return;
        }
        const uint64_t source_hash = core::hash_bytes(bytes->data(), bytes->size());
        Hasher hasher("texture");
        hasher.add(source.id.value());
        hasher.add(asset::TextureAsset::kFormatVersion);
        hasher.add(source_hash);
        hasher.add(std::as_bytes(settings_json.write(false).span()));
        if (const CookRecord* same = unchanged(path, hasher.value()); same != nullptr) {
            keep(*same);
            return;
        }
        CookRecord record{
            .path = source.path, .guid = source.guid, .source_hash = source_hash, .input_hash = hasher.value()};
        const core::Result<Image> image = decode_image(bytes->span(), path, diagnostics_);
        core::Result<core::Array<std::byte>> cooked =
            image ? cook_texture(*image, settings, source.id, &jobs_, path, diagnostics_)
                  : core::Result<core::Array<std::byte>>(core::fail(image.error()));
        if (!cooked || !write(record, source.id, asset::AssetType::Texture, *cooked)) {
            failed(path);
            return;
        }
        next_.put(std::move(record));
        ++stats_.cooked;
    }

    // Cooked files no source makes any more: their source was deleted, or no longer has them.
    void remove_stale() {
        core::HashMap<uint64_t, uint8_t> live(tools_heap());
        for (const CookRecord& record : next_.records()) {
            for (const CookedOutput& output : record.outputs) {
                live.set(output.id.value(), 1);
            }
        }
        for (const CookRecord& record : previous_.records()) {
            for (const CookedOutput& output : record.outputs) {
                if (live.find(output.id.value()) != nullptr) {
                    continue;
                }
                live.set(output.id.value(), 1); // once
                const asset::CookedPath path = asset::cooked_path(output.id, output.type);
                if (vfs_.remove_file(path.c_str()).ok()) {
                    ++stats_.removed;
                } else {
                    diagnostics_.warning(record.path.view(), "its old cooked file %s could not be removed",
                                         path.c_str());
                }
            }
        }
    }

    platform::Vfs& vfs_;
    core::JobSystem& jobs_;
    shader::ShaderCompiler* compiler_;
    const CookOptions& options_;
    Diagnostics diagnostics_;
    CookStats stats_;
    CookState previous_;
    CookState next_;
    SourceTree tree_;
    core::HashMap<uint64_t, TextureUsage> usages_{tools_heap()};  // texture path -> how it is used
    core::HashMap<uint64_t, uint64_t> written_{tools_heap()};     // asset ID -> source path key, this run
    core::HashMap<uint64_t, uint64_t> file_hashes_{tools_heap()}; // path key -> content hash, this run
};

} // namespace

bool CookReport::ok() const {
    return std::ranges::none_of(diagnostics,
                                [](const Diagnostic& diagnostic) { return diagnostic.severity == Severity::Error; });
}

CookReport cook(platform::Vfs& vfs, core::JobSystem& jobs, shader::ShaderCompiler* compiler,
                const CookOptions& options) {
    DE_PROFILE_ZONE("cook");
    init_third_party();
    Cook cook(vfs, jobs, compiler, options);
    return cook.run();
}

core::Array<char> report_json(const CookReport& report) {
    const JsonBuilder json;
    yyjson_mut_doc* doc = json.doc();
    yyjson_mut_val* root = json.root();
    yyjson_mut_obj_add_bool(doc, root, "ok", report.ok());
    yyjson_mut_val* stats = yyjson_mut_obj_add_obj(doc, root, "stats");
    yyjson_mut_obj_add_uint(doc, stats, "cooked", report.stats.cooked);
    yyjson_mut_obj_add_uint(doc, stats, "up_to_date", report.stats.up_to_date);
    yyjson_mut_obj_add_uint(doc, stats, "failed", report.stats.failed);
    yyjson_mut_obj_add_uint(doc, stats, "removed", report.stats.removed);
    yyjson_mut_obj_add_uint(doc, stats, "metas_created", report.stats.metas_created);
    yyjson_mut_obj_add_uint(doc, stats, "metas_relinked", report.stats.metas_relinked);
    yyjson_mut_val* diagnostics = yyjson_mut_obj_add_arr(doc, root, "diagnostics");
    for (const Diagnostic& diagnostic : report.diagnostics) {
        yyjson_mut_val* item = yyjson_mut_arr_add_obj(doc, diagnostics);
        yyjson_mut_obj_add_str(doc, item, "severity", diagnostic.severity == Severity::Error ? "error" : "warning");
        yyjson_mut_obj_add_strncpy(doc, item, "path", diagnostic.path.c_str(), diagnostic.path.size());
        yyjson_mut_obj_add_strncpy(doc, item, "message", diagnostic.message.c_str(), diagnostic.message.size());
    }
    return json.write(true);
}

} // namespace de::cooker
