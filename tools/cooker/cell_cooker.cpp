#include "cooker/cell_cooker.h"

#include "cooker/blob_writer.h"
#include "cooker/json.h"
#include "cooker/source_tree.h"
#include "core/hash_map.h"
#include "core/math.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace de::cooker {

namespace {

using asset::CellEntity;

// count numbers from an array of exactly that many; false otherwise.
template <typename T>
bool read_numbers(yyjson_val* value, T* out, size_t count) {
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) != count) {
        return false;
    }
    size_t i = 0;
    size_t n = 0;
    yyjson_val* item = nullptr;
    yyjson_arr_foreach(value, i, n, item) {
        if (!yyjson_is_num(item)) {
            return false;
        }
        out[i] = static_cast<T>(yyjson_get_num(item));
    }
    return true;
}

enum class StaticSetting : uint8_t { Unset, Yes, No };

// One entity as read, before parents are put first.
struct Parsed {
    CellEntity entity;
    std::string_view name;
    std::string_view parent;     // its name, resolved once every entity is read
    bool parent_missing = false; // named a parent that isn't there (reported)
    uint32_t depth = 0;          // parents to the root
    StaticSetting static_setting = StaticSetting::Unset;
};

// A float from a JSON number, if it fits.
bool to_float(double value, float& out) {
    if (!std::isfinite(value) || std::abs(value) > static_cast<double>(std::numeric_limits<float>::max())) {
        return false;
    }
    out = static_cast<float>(value);
    return true;
}

class CellImport {
public:
    CellImport(const SourceTree& tree, std::string_view path, Diagnostics& diagnostics)
        : tree_(tree), path_(path), diagnostics_(diagnostics) {}

    core::Result<CellBuild> run(yyjson_val* root) {
        const size_t errors_before = diagnostics_.error_count();
        const JsonReader reader{.object = root, .path = path_, .diagnostics = &diagnostics_};
        static constexpr const char* kKeys[] = {"coord", "entities"};
        reader.check_keys(kKeys, "cell");
        int32_t coord[2] = {};
        double coord_read[2] = {};
        if (!read_numbers(yyjson_obj_get(root, "coord"), coord_read, 2) || coord_read[0] != std::floor(coord_read[0]) ||
            coord_read[1] != std::floor(coord_read[1]) || std::abs(coord_read[0]) > 1e6 ||
            std::abs(coord_read[1]) > 1e6) {
            diagnostics_.error(path_, "coord should be [x, z]: two whole numbers");
        } else {
            coord[0] = static_cast<int32_t>(coord_read[0]);
            coord[1] = static_cast<int32_t>(coord_read[1]);
        }
        build_.coord_x = coord[0];
        build_.coord_z = coord[1];

        yyjson_val* entities = yyjson_obj_get(root, "entities");
        if (!yyjson_is_arr(entities)) {
            diagnostics_.error(path_, "entities should be an array");
        }
        size_t i = 0;
        size_t n = 0;
        yyjson_val* item = nullptr;
        yyjson_arr_foreach(entities, i, n, item) {
            read_entity(item, i);
        }
        resolve_parents();
        resolve_static();
        if (diagnostics_.error_count() != errors_before) {
            return core::fail(core::ErrorCode::InvalidArgument, "the cell has errors");
        }
        write_in_order();
        return std::move(build_);
    }

private:
    void read_entity(yyjson_val* object, size_t index) {
        parsed_.emplace(); // one per source entry, so indices match the source's
        if (!yyjson_is_obj(object)) {
            diagnostics_.error(path_, "entity %zu should be an object", index);
            return;
        }
        const JsonReader reader{.object = object, .path = path_, .diagnostics = &diagnostics_};
        static constexpr const char* kKeys[] = {"name",         "mesh",  "position", "rotation",
                                                "rotation_deg", "scale", "parent",   "static"};
        reader.check_keys(kKeys, "cell entity");
        Parsed& parsed = parsed_.back();
        parsed.name = reader.string("name", "");
        parsed.parent = reader.string("parent", "");
        const Label label = label_of(index);
        const auto bad = [&](const char* what) { diagnostics_.error(path_, "%s: %s", label.c_str(), what); };

        if (parsed.name.size() > UINT16_MAX) {
            bad("its name is too long");
        }
        if (!parsed.name.empty()) {
            if (find_name(parsed.name) != CellEntity::kNone) {
                bad("another entity has its name");
            } else {
                by_name_.set(name_key(parsed.name), static_cast<uint32_t>(index));
            }
        }

        CellEntity& entity = parsed.entity;
        if (reader.has("position") && !read_numbers(yyjson_obj_get(object, "position"), entity.position, 3)) {
            bad("position should be [x, y, z]");
        }
        if (reader.has("rotation") && reader.has("rotation_deg")) {
            bad("has both rotation and rotation_deg");
        } else if (reader.has("rotation")) {
            double q[4] = {};
            const bool read = read_numbers(yyjson_obj_get(object, "rotation"), q, 4);
            // In doubles: large components would overflow a float's square.
            const double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (!read || !std::isfinite(norm) || !(norm > 1e-6)) {
                bad("rotation should be a quaternion [x, y, z, w], not all zero");
            } else {
                for (size_t c = 0; c < 4; ++c) {
                    entity.rotation[c] = static_cast<float>(q[c] / norm);
                }
            }
        } else if (reader.has("rotation_deg")) {
            double read_degrees[3] = {};
            float degrees[3] = {};
            if (!read_numbers(yyjson_obj_get(object, "rotation_deg"), read_degrees, 3) ||
                !(std::abs(read_degrees[0]) <= 1e6 && std::abs(read_degrees[1]) <= 1e6 &&
                  std::abs(read_degrees[2]) <= 1e6)) {
                bad("rotation_deg should be [x, y, z] in degrees");
            } else {
                for (size_t c = 0; c < 3; ++c) {
                    degrees[c] = static_cast<float>(read_degrees[c]);
                }
                const Quat q = glm::angleAxis(glm::radians(degrees[1]), Vec3(0.0f, 1.0f, 0.0f)) *
                               glm::angleAxis(glm::radians(degrees[0]), Vec3(1.0f, 0.0f, 0.0f)) *
                               glm::angleAxis(glm::radians(degrees[2]), Vec3(0.0f, 0.0f, 1.0f));
                const Quat unit = glm::normalize(q);
                entity.rotation[0] = unit.x;
                entity.rotation[1] = unit.y;
                entity.rotation[2] = unit.z;
                entity.rotation[3] = unit.w;
            }
        }
        if (yyjson_val* scale = yyjson_obj_get(object, "scale"); scale != nullptr) {
            double read_scale[3] = {};
            if (yyjson_is_num(scale)) {
                read_scale[0] = read_scale[1] = read_scale[2] = yyjson_get_num(scale);
            } else if (!read_numbers(scale, read_scale, 3)) {
                bad("scale should be a number or [x, y, z]");
            }
            if (!to_float(read_scale[0], entity.scale[0]) || !to_float(read_scale[1], entity.scale[1]) ||
                !to_float(read_scale[2], entity.scale[2])) {
                bad("scale is too large for a float");
            } else if (entity.scale[0] == 0.0f || entity.scale[1] == 0.0f || entity.scale[2] == 0.0f) {
                bad("scale has a zero (it couldn't be undone, for attaching)");
            }
        }
        if (reader.has("static")) {
            parsed.static_setting = reader.boolean("static", true) ? StaticSetting::Yes : StaticSetting::No;
        }

        if (reader.has("mesh")) {
            const std::string_view reference = reader.string("mesh", "");
            SourcePath mesh_path;
            const Source* mesh = nullptr;
            if (join_relative(folder_of(path_), reference, mesh_path)) {
                mesh = tree_.find(mesh_path.view());
            }
            if (mesh == nullptr || (mesh->kind != SourceKind::Gltf && mesh->kind != SourceKind::Fbx)) {
                diagnostics_.error(path_,
                                   "%s: mesh '%.*s' should name a model source (.gltf, .glb, .fbx), relative to "
                                   "the cell's folder",
                                   label.c_str(), static_cast<int>(reference.size()), reference.data());
            } else {
                entity.mesh = mesh_index(*mesh);
            }
        }
    }

    // How messages name an entity: its name, or "entity <index in the source>".
    using Label = core::FixedString<64>;
    Label label_of(size_t index) const {
        Label label;
        const std::string_view name = parsed_[index].name;
        if (name.empty()) {
            static_cast<void>(label.append("entity "));
            static_cast<void>(label.append(number_text(index).view()));
        } else {
            static_cast<void>(label.append(name.substr(0, 60)));
        }
        return label;
    }

    static uint64_t name_key(std::string_view name) { return core::hash_bytes(name.data(), name.size()); }

    // The entity with this name, read so far; kNone if there's none.
    uint32_t find_name(std::string_view name) const {
        const uint32_t* found = by_name_.find(name_key(name));
        if (found != nullptr && parsed_[*found].name == name) {
            return *found;
        }
        if (found == nullptr) {
            return CellEntity::kNone;
        }
        // Two names with one hash: look it up the slow way.
        for (size_t i = 0; i < parsed_.size(); ++i) {
            if (parsed_[i].name == name) {
                return static_cast<uint32_t>(i);
            }
        }
        return CellEntity::kNone;
    }

    uint32_t mesh_index(const Source& mesh) {
        const asset::AssetRef ref{.id = mesh.id, .type = asset::AssetType::Mesh};
        for (size_t i = 0; i < build_.meshes.size(); ++i) {
            if (build_.meshes[i] == ref) {
                return static_cast<uint32_t>(i);
            }
        }
        build_.meshes.push(ref);
        build_.mesh_sources.push(mesh.path);
        return static_cast<uint32_t>(build_.meshes.size() - 1);
    }

    // Parents by name, then each entity's depth (a loop is an error), and roots in their cell.
    void resolve_parents() {
        for (size_t i = 0; i < parsed_.size(); ++i) {
            Parsed& parsed = parsed_[i];
            if (parsed.parent.empty()) {
                continue;
            }
            const uint32_t found = find_name(parsed.parent);
            if (found == CellEntity::kNone || found == i) {
                diagnostics_.error(path_, "%s: parent '%.*s' isn't another entity of the cell", label_of(i).c_str(),
                                   static_cast<int>(parsed.parent.size()), parsed.parent.data());
                parsed.parent_missing = true;
                continue;
            }
            parsed.entity.parent = found; // source index for now
        }
        const double cell_size = asset::kCellSize;
        for (size_t i = 0; i < parsed_.size(); ++i) {
            Parsed& parsed = parsed_[i];
            uint32_t depth = 0;
            uint32_t at = parsed.entity.parent;
            while (at != CellEntity::kNone && depth <= parsed_.size()) {
                ++depth;
                at = parsed_[at].entity.parent;
            }
            if (depth > parsed_.size()) {
                // Its chain runs into a loop: reported for the entities on it.
                if (on_loop(i)) {
                    diagnostics_.error(path_, "%s: its parents loop back to it", label_of(i).c_str());
                }
                continue;
            }
            parsed.depth = depth;
            const double x = parsed.entity.position[0] / cell_size - build_.coord_x;
            const double z = parsed.entity.position[2] / cell_size - build_.coord_z;
            if (depth == 0 && !parsed.parent_missing && (x < 0.0 || x >= 1.0 || z < 0.0 || z >= 1.0)) {
                diagnostics_.warning(path_, "%s: lies outside the cell [%d, %d]", label_of(i).c_str(), build_.coord_x,
                                     build_.coord_z);
            }
        }
    }

    // Whether following parents from entity i comes back to it.
    bool on_loop(size_t i) const {
        uint32_t at = parsed_[i].entity.parent;
        for (size_t steps = 0; steps < parsed_.size() && at != CellEntity::kNone; ++steps) {
            if (at == i) {
                return true;
            }
            at = parsed_[at].entity.parent;
        }
        return false;
    }

    // Parents first: by depth, otherwise in source order.
    void sort_by_depth() {
        order_.clear();
        for (size_t i = 0; i < parsed_.size(); ++i) {
            order_.push(static_cast<uint32_t>(i));
        }
        std::ranges::stable_sort(order_, [&](uint32_t a, uint32_t b) { return parsed_[a].depth < parsed_[b].depth; });
    }

    // Static unless said otherwise, and as its parent: a child of something that moves moves with it,
    // so it can't be static (StaticTransform promises it never moves).
    void resolve_static() {
        sort_by_depth();
        for (const uint32_t i : order_) {
            Parsed& parsed = parsed_[i];
            const uint32_t parent = parsed.entity.parent;
            const bool parent_static = parent == CellEntity::kNone ||
                                       core::has_any(parsed_[parent].entity.flags, asset::CellEntityFlags::Static);
            bool is_static = parent_static;
            if (parsed.static_setting == StaticSetting::No) {
                is_static = false;
            } else if (parsed.static_setting == StaticSetting::Yes && !parent_static) {
                diagnostics_.error(path_, "%s: can't be static: its parent '%.*s' moves", label_of(i).c_str(),
                                   static_cast<int>(parsed.parent.size()), parsed.parent.data());
            }
            parsed.entity.flags = is_static ? asset::CellEntityFlags::Static : asset::CellEntityFlags::None;
        }
    }

    // Parents first, with parent indices remapped.
    void write_in_order() {
        core::Array<uint32_t> written_at(tools_heap());
        written_at.resize(parsed_.size());
        for (size_t i = 0; i < order_.size(); ++i) {
            written_at[order_[i]] = static_cast<uint32_t>(i);
        }
        for (const uint32_t source_index : order_) {
            const Parsed& parsed = parsed_[source_index];
            CellEntity entity = parsed.entity;
            if (entity.parent != CellEntity::kNone) {
                entity.parent = written_at[entity.parent];
            }
            entity.name_offset = static_cast<uint32_t>(build_.names.size());
            entity.name_length = static_cast<uint16_t>(parsed.name.size());
            for (const char c : parsed.name) {
                build_.names.push(c);
            }
            build_.entities.push(entity);
        }
    }

    const SourceTree& tree_;
    std::string_view path_;
    Diagnostics& diagnostics_;
    CellBuild build_;
    core::Array<Parsed> parsed_{tools_heap()};
    core::HashMap<uint64_t, uint32_t> by_name_{tools_heap()}; // name hash -> first entity with it
    core::Array<uint32_t> order_{tools_heap()};               // source indices, parents first
};

} // namespace

core::Result<CellBuild> import_cell(const SourceTree& tree, const Source& source, std::span<const std::byte> bytes,
                                    Diagnostics& diagnostics) {
    const std::string_view path = source.path.view();
    const core::Result<JsonDoc> doc = JsonDoc::parse(bytes, path, diagnostics);
    if (!doc) {
        return core::fail(doc.error());
    }
    if (!yyjson_is_obj(doc->root())) {
        diagnostics.error(path, "should be a JSON object");
        return core::fail(core::ErrorCode::InvalidArgument, "the cell has errors");
    }
    CellImport import(tree, path, diagnostics);
    return import.run(doc->root());
}

core::Array<std::byte> cook_cell(const CellBuild& build, asset::AssetId id) {
    asset::CellAsset asset;
    asset.coord_x = build.coord_x;
    asset.coord_z = build.coord_z;
    BlobWriter writer(sizeof(asset::CellAsset));
    asset.entities = writer.append(build.entities.span());
    asset.names = writer.append(build.names.span());
    return writer.finish(asset, id, build.meshes.span());
}

} // namespace de::cooker
