#include "cooker/cell_cooker.h"

#include "cooker/blob_writer.h"
#include "cooker/json.h"
#include "cooker/source_tree.h"
#include "core/hash_map.h"
#include "core/math.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>

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

// One entity as read, before parents are put first.
struct Parsed {
    CellEntity entity;
    std::string_view name;
    std::string_view parent; // its name, resolved once every entity is read
    uint32_t depth = 0;      // parents to the root
};

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
        for (const Parsed& other : parsed_.span().first(parsed_.size() - 1)) {
            if (!parsed.name.empty() && other.name == parsed.name) {
                bad("another entity has its name");
                break;
            }
        }

        CellEntity& entity = parsed.entity;
        if (reader.has("position") && !read_numbers(yyjson_obj_get(object, "position"), entity.position, 3)) {
            bad("position should be [x, y, z]");
        }
        if (reader.has("rotation") && reader.has("rotation_deg")) {
            bad("has both rotation and rotation_deg");
        } else if (reader.has("rotation")) {
            float q[4] = {};
            const bool read = read_numbers(yyjson_obj_get(object, "rotation"), q, 4);
            const float norm = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
            if (!read || !(norm > 1e-6f)) {
                bad("rotation should be a quaternion [x, y, z, w], not all zero");
            } else {
                for (size_t c = 0; c < 4; ++c) {
                    entity.rotation[c] = q[c] / norm;
                }
            }
        } else if (reader.has("rotation_deg")) {
            float degrees[3] = {};
            if (!read_numbers(yyjson_obj_get(object, "rotation_deg"), degrees, 3)) {
                bad("rotation_deg should be [x, y, z] in degrees");
            } else {
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
            if (yyjson_is_num(scale)) {
                const auto uniform = static_cast<float>(yyjson_get_num(scale));
                entity.scale[0] = uniform;
                entity.scale[1] = uniform;
                entity.scale[2] = uniform;
            } else if (!read_numbers(scale, entity.scale, 3)) {
                bad("scale should be a number or [x, y, z]");
            }
            if (entity.scale[0] == 0.0f || entity.scale[1] == 0.0f || entity.scale[2] == 0.0f) {
                bad("scale has a zero (it couldn't be undone, for attaching)");
            }
        }
        entity.flags = reader.boolean("static", true) ? asset::CellEntityFlags::Static : asset::CellEntityFlags::None;

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
            const auto* found = std::ranges::find_if(
                parsed_, [&](const Parsed& other) { return !other.name.empty() && other.name == parsed.parent; });
            if (found == parsed_.end() || found == &parsed) {
                diagnostics_.error(path_, "%s: parent '%.*s' isn't another entity of the cell", label_of(i).c_str(),
                                   static_cast<int>(parsed.parent.size()), parsed.parent.data());
                continue;
            }
            parsed.entity.parent = static_cast<uint32_t>(found - parsed_.begin()); // source index for now
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
                diagnostics_.error(path_, "%s: its parents loop back to it", label_of(i).c_str());
                continue;
            }
            parsed.depth = depth;
            const double x = parsed.entity.position[0] / cell_size - build_.coord_x;
            const double z = parsed.entity.position[2] / cell_size - build_.coord_z;
            if (depth == 0 && (x < 0.0 || x >= 1.0 || z < 0.0 || z >= 1.0)) {
                diagnostics_.warning(path_, "%s: lies outside the cell [%d, %d]", label_of(i).c_str(), build_.coord_x,
                                     build_.coord_z);
            }
        }
    }

    // Parents first (by depth, otherwise in source order), with parent indices remapped.
    void write_in_order() {
        core::Array<uint32_t> order(tools_heap());
        for (size_t i = 0; i < parsed_.size(); ++i) {
            order.push(static_cast<uint32_t>(i));
        }
        std::ranges::stable_sort(order, [&](uint32_t a, uint32_t b) { return parsed_[a].depth < parsed_[b].depth; });
        core::Array<uint32_t> written_at(tools_heap());
        written_at.resize(parsed_.size());
        for (size_t i = 0; i < order.size(); ++i) {
            written_at[order[i]] = static_cast<uint32_t>(i);
        }
        for (const uint32_t source_index : order) {
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
