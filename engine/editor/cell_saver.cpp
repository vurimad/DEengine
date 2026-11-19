#include "editor/cell_saver.h"

#include "asset/cell_format.h"
#include "asset/prefab_format.h"
#include "asset/registry.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/log.h"
#include "core/profile.h"
#include "editor/cell_source.h"
#include "editor/entity_record.h"
#include "editor/source_files.h"
#include "platform/vfs.h"
#include "world/cell.h"
#include "world/prefab.h"
#include "world/render_extract.h"
#include "world/transform.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <span>
#include <string_view>

namespace de::editor {

namespace {

core::Allocator& saver_heap() {
    return core::heap_allocator(core::MemoryTag::Editor);
}

using Name = core::FixedString<128>;

std::string_view folder_of(std::string_view path) {
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(0, slash);
}

// Every file under a folder of src:/content, into visit (its src:/ path).
template <typename Visit>
bool list_sources(const platform::Vfs& vfs, const platform::PathString& folder, int depth, Visit& visit) {
    if (depth > 32) {
        return true;
    }
    bool ok = true;
    const core::Result<void> listed = vfs.list(folder.c_str(), [&](const char* name, bool is_directory) {
        platform::PathString path;
        if (!path.assign(folder.view()) || !path.append("/") || !path.append(name)) {
            return;
        }
        if (is_directory) {
            ok = list_sources(vfs, path, depth + 1, visit) && ok;
        } else {
            visit(path);
        }
    });
    if (!listed) {
        DE_LOG_WARN("editor", "can't look through %s: %s", folder.c_str(), listed.error().message);
        return false;
    }
    return ok;
}

bool read_text(const platform::Vfs& vfs, const char* path, core::Array<char>& out) {
    const auto bytes = vfs.read_whole_file(path, saver_heap());
    if (!bytes) {
        return false;
    }
    out.resize(bytes->size());
    std::memcpy(out.data(), bytes->data(), bytes->size());
    return true;
}

uint64_t pack_key(const MemberKey& key) {
    return (static_cast<uint64_t>(static_cast<uint16_t>(key.x)) << 48U) |
           (static_cast<uint64_t>(static_cast<uint16_t>(key.z)) << 32U) | key.source_index;
}

// A member of a loaded cell: which of the cooked cell's entities it is.
struct Member {
    flecs::entity_t cell = 0;
    uint32_t index = 0;
    flecs::entity_t entity = 0;
};

bool same_position(const DVec3& a, const double (&b)[3]) {
    constexpr double kTolerance = 1e-5; // well under what a file writes (0.1 mm)
    return std::abs(a.x - b[0]) <= kTolerance && std::abs(a.y - b[1]) <= kTolerance &&
           std::abs(a.z - b[2]) <= kTolerance;
}

bool same_rotation(const Quat& a, const float (&b)[4]) {
    const float dot = (a.x * b[0]) + (a.y * b[1]) + (a.z * b[2]) + (a.w * b[3]);
    return std::abs(dot) >= 1.0f - 1e-7f;
}

bool same_floats(const float* a, const float* b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (std::abs(a[i] - b[i]) > 1e-6f * std::max(1.0f, std::abs(b[i]))) {
            return false;
        }
    }
    return true;
}

// A fog volume's key, written as its file writes it.
struct FogEdit {
    const char* key = "";
    char value[96] = {};
    int decimals = 4; // -1: compared as text
};

void number_list(const float* values, size_t count, std::span<char> out) {
    size_t length = 0;
    out[length++] = '[';
    for (size_t i = 0; i < count; ++i) {
        if (i > 0) {
            out[length++] = ',';
            out[length++] = ' ';
        }
        length += format_number(values[i], 4, out.subspan(length));
    }
    out[length++] = ']';
    out[length] = '\0';
}

// What differs between a fog volume as cooked and as it is now, key by key.
size_t fog_edits(const asset::CellFogVolume& cooked, const world::FogVolume& now, std::span<FogEdit> out) {
    size_t count = 0;
    const auto number = [&](const char* key, float was, float is) {
        if (!same_floats(&is, &was, 1) && count < out.size()) {
            FogEdit& edit = out[count++];
            edit.key = key;
            edit.value[format_number(is, 4, edit.value)] = '\0';
        }
    };
    const auto list = [&](const char* key, const float* was, const float* is) {
        if (!same_floats(is, was, 3) && count < out.size()) {
            FogEdit& edit = out[count++];
            edit.key = key;
            number_list(is, 3, edit.value);
        }
    };
    const bool was_ellipsoid = cooked.shape == asset::CellFogShape::Ellipsoid;
    if (was_ellipsoid != now.ellipsoid && count < out.size()) {
        FogEdit& edit = out[count++];
        edit.key = "shape";
        static_cast<void>(std::snprintf(edit.value, sizeof(edit.value), "\"%s\"", now.ellipsoid ? "ellipsoid" : "box"));
        edit.decimals = -1;
    }
    number("density_per_m", cooked.density_per_m, now.density_per_m);
    number("edge_m", cooked.edge_m, now.edge_m);
    list("tint", cooked.tint, now.tint);
    number("noise_size_m", cooked.noise_size_m, now.noise_size_m);
    number("noise_amount", cooked.noise_amount, now.noise_amount);
    list("drift_mps", cooked.drift_mps, now.drift_mps);
    number("height_fade_m", cooked.height_fade_m, now.height_fade_m);
    number("under_roof", cooked.under_roof, now.under_roof);
    return count;
}

// Whether a light is what its cooked cell has.
bool same_light(const asset::LightDesc& cooked, const world::LocalLight& now) {
    const asset::LightDesc is = now.desc();
    return same_floats(is.color, cooked.color, 3) && same_floats(&is.intensity, &cooked.intensity, 1) &&
           same_floats(&is.radius_m, &cooked.radius_m, 1) && same_floats(&is.cone_deg, &cooked.cone_deg, 1) &&
           same_floats(&is.cone_soft, &cooked.cone_soft, 1) && same_floats(&is.flicker, &cooked.flicker, 1) &&
           same_floats(&is.flicker_hz, &cooked.flicker_hz, 1) && same_floats(&is.shadow, &cooked.shadow, 1);
}

// An entity's light as its entry writes it: the whole object, in the one layout a light is written in.
void write_light(CellSource& source, uint32_t index, const world::LocalLight& light) {
    char text[kLightTextSize];
    const size_t length = light_text(light.desc(), text);
    static_cast<void>(source.set_entity_member(index, "light", {text, length}));
}

// A folder as an entry writes it: removed when it's empty, else set (quoted).
void write_folder(CellSource& source, uint32_t index, const char* path) {
    if (path[0] == 0) {
        static_cast<void>(source.remove_entity_member(index, "folder"));
        return;
    }
    char quoted[160];
    const int length = std::snprintf(quoted, sizeof(quoted), "\"%s\"", path);
    if (length > 0 && static_cast<size_t>(length) < sizeof(quoted)) {
        static_cast<void>(source.set_entity_member(index, "folder", {quoted, static_cast<size_t>(length)}));
    }
}

// The variant a placement asks for, as an entry writes it: removed when it asks for none (or for one its prefab
// doesn't have: there's no name to write), else its name (quoted).
void write_variant(CellSource& source, uint32_t index, const asset::AssetRegistry& assets,
                   const world::PrefabInstance& placed) {
    const auto* prefab = assets.get<asset::PrefabAsset>(placed.prefab);
    if (placed.variant != 0 && prefab != nullptr) {
        for (const asset::PrefabVariant& variant : prefab->variants.get(prefab)) {
            if (variant.name_hash != placed.variant) {
                continue;
            }
            const std::string_view name = prefab->name(prefab, variant);
            char quoted[64];
            const int length =
                std::snprintf(quoted, sizeof(quoted), "\"%.*s\"", static_cast<int>(name.size()), name.data());
            if (length > 0 && static_cast<size_t>(length) < sizeof(quoted)) {
                static_cast<void>(source.set_entity_member(index, "variant", {quoted, static_cast<size_t>(length)}));
            }
            return;
        }
    }
    static_cast<void>(source.remove_entity_member(index, "variant"));
}

// A cell file open while saving, edited in memory and written at the end.
struct OpenFile {
    explicit OpenFile(core::Allocator& allocator) : source(allocator), removed(allocator) {}
    platform::PathString path;
    CellSource source;
    int32_t x = 0;
    int32_t z = 0;
    uint32_t world = UINT32_MAX; // the world it's filed in (UINT32_MAX: none lists it)
    bool changed = false;
    bool made = false;             // a new file, for a cell that had none
    bool received = false;         // things moved into it from another cell's file
    core::Array<uint32_t> removed; // places taken out (as they were before any was)
};

// One entry moving from its file to another cell's.
struct Moving {
    uint32_t from_file = 0;
    uint32_t from_index = 0;
    uint32_t to_file = 0;
    Name name;
    Name parent;
    bool parent_renamed = false; // its parent took another name where they go
    core::Array<char> text{saver_heap()};
};

} // namespace

// ---- CellSources ----

bool CellSources::scan(const platform::Vfs& vfs) {
    DE_PROFILE_ZONE("CellSources::scan");
    cells_.clear();
    worlds_.clear();
    listed_.clear();
    by_id_.clear();
    platform::PathString root;
    static_cast<void>(root.assign("src:/content"));
    core::Array<char> text(saver_heap());
    CellSource reader(saver_heap());
    core::Array<platform::PathString> world_paths(saver_heap());
    auto visit = [&](const platform::PathString& path) {
        if (path.view().ends_with(".world")) {
            world_paths.push(path);
            return;
        }
        if (!path.view().ends_with(".cell")) {
            return;
        }
        asset::Guid guid;
        Cell cell;
        if (!source_asset_id(vfs, path.view(), guid, cell.id)) {
            return; // not cooked yet: nothing loaded came from it
        }
        cell.path = path;
        if (read_text(vfs, path.c_str(), text) && reader.parse({text.data(), text.size()})) {
            cell.placed = reader.coord(cell.x, cell.z);
        }
        by_id_.set(cell.id.value(), static_cast<uint32_t>(cells_.size()));
        cells_.push(cell);
    };
    const bool listed = list_sources(vfs, root, 0, visit);
    for (const platform::PathString& path : world_paths) {
        World world;
        asset::Guid guid;
        if (!source_asset_id(vfs, path.view(), guid, world.id) || !read_text(vfs, path.c_str(), text)) {
            continue;
        }
        world.path = path;
        const auto index = static_cast<uint32_t>(worlds_.size());
        worlds_.push(world);
        static_cast<void>(world_cells({text.data(), text.size()}, [&](std::string_view cell) {
            char joined[platform::kMaxPathLength];
            size_t length = 0;
            if (!joined_path(folder_of(path.view()), cell, joined, length)) {
                return;
            }
            const std::string_view full(joined, length);
            for (size_t i = 0; i < cells_.size(); ++i) {
                if (cells_[i].path.view() == full) {
                    listed_.push({.world = index, .cell = static_cast<uint32_t>(i)});
                }
            }
        }));
    }
    return listed;
}

const CellSources::Cell* CellSources::find(asset::AssetId id) const {
    const uint32_t* index = by_id_.find(id.value());
    return index != nullptr ? &cells_[*index] : nullptr;
}

uint32_t CellSources::world_index(asset::AssetId id) const {
    for (size_t i = 0; i < worlds_.size(); ++i) {
        if (worlds_[i].id == id) {
            return static_cast<uint32_t>(i);
        }
    }
    return UINT32_MAX;
}

uint32_t CellSources::world_of(std::string_view cell_path, asset::AssetId wanted) const {
    uint32_t first = UINT32_MAX;
    for (const Listing& listing : listed_) {
        if (cells_[listing.cell].path.view() != cell_path) {
            continue;
        }
        if (worlds_[listing.world].id == wanted) {
            return listing.world;
        }
        first = std::min(first, listing.world);
    }
    return first;
}

const CellSources::Cell* CellSources::cell_at(uint32_t world, int32_t x, int32_t z) const {
    for (const Listing& listing : listed_) {
        const Cell& cell = cells_[listing.cell];
        if (listing.world == world && cell.placed && cell.x == x && cell.z == z) {
            return &cell;
        }
    }
    return nullptr;
}

// ---- Saving ----

CellSaveReport save_cells(const flecs::world& ecs, const asset::AssetRegistry& assets, const platform::Vfs& vfs,
                          const CellSources& sources, asset::AssetId world, core::Array<platform::PathString>* written,
                          core::Array<MemberMove>* moves, std::span<const MemberRemoval> removed,
                          core::Array<PlacedMember>* placed_out) {
    DE_PROFILE_ZONE("save_cells");
    CellSaveReport report;

    // Every member of a loaded cell, grouped by cell.
    core::Array<Member> members(saver_heap());
    ecs.each([&](flecs::entity entity, const world::CellMember& member) {
        const flecs::entity cell = entity.target<world::InCell>();
        if (cell.is_valid()) {
            members.push({.cell = cell.id(), .index = member.index, .entity = entity.id()});
        }
    });
    std::ranges::sort(members, [](const Member& a, const Member& b) {
        return a.cell != b.cell ? a.cell < b.cell : a.index < b.index;
    });
    // And by where each is in its file.
    core::HashMap<uint64_t, flecs::entity_t> by_key(saver_heap());
    for (const Member& member : members) {
        const auto* held = ecs.entity(member.cell).try_get<world::Cell>();
        const auto* cooked = held != nullptr ? assets.get<asset::CellAsset>(held->asset) : nullptr;
        if (cooked != nullptr && member.index < cooked->entities.count) {
            by_key.set(pack_key({.x = cooked->coord_x,
                                 .z = cooked->coord_z,
                                 .source_index = cooked->entities.get(cooked)[member.index].source_index}),
                       member.entity);
        }
    }

    core::Array<OpenFile> files(saver_heap());
    core::Array<char> text(saver_heap());
    const auto open_file = [&](const char* path) -> uint32_t {
        for (size_t i = 0; i < files.size(); ++i) {
            if (files[i].path.view() == path) {
                return static_cast<uint32_t>(i);
            }
        }
        if (!read_text(vfs, path, text)) {
            DE_LOG_ERROR("editor", "can't read %s", path);
            return UINT32_MAX;
        }
        OpenFile& file = files.push(OpenFile(saver_heap()));
        static_cast<void>(file.path.assign(path));
        if (const core::Result<void> parsed = file.source.parse({text.data(), text.size()}); !parsed) {
            DE_LOG_ERROR("editor", "can't save into %s: %s", path, parsed.error().message);
            files.pop();
            return UINT32_MAX;
        }
        static_cast<void>(file.source.coord(file.x, file.z));
        file.world = sources.world_of(file.path.view(), world);
        return static_cast<uint32_t>(files.size() - 1);
    };

    // 1. Each cell's changes, into its file; and what now stands in another cell's square.
    struct Refile {
        uint32_t file = 0;
        uint32_t source_index = 0;
        int32_t x = 0;
        int32_t z = 0;
    };
    core::Array<Refile> refiles(saver_heap());
    core::Array<flecs::entity_t> by_index(saver_heap());
    for (size_t first = 0; first < members.size();) {
        size_t last = first;
        while (last < members.size() && members[last].cell == members[first].cell) {
            ++last;
        }
        const std::span<const Member> group(members.data() + first, last - first);
        first = last;

        const flecs::entity cell = ecs.entity(group[0].cell);
        const auto* held = cell.try_get<world::Cell>();
        const asset::CellAsset* cooked = held != nullptr ? assets.get<asset::CellAsset>(held->asset) : nullptr;
        if (cooked == nullptr) {
            continue; // not loaded now: nothing of it is shown, so nothing was edited
        }
        const std::span<const asset::CellEntity> entities = cooked->entities.get(cooked);
        const std::span<const asset::CellFogVolume> fogs = cooked->fog_volumes.get(cooked);
        by_index.clear();
        by_index.resize(entities.size(), 0);
        for (const Member& member : group) {
            if (member.index < entities.size()) {
                by_index[member.index] = member.entity;
            }
        }

        struct Change {
            uint32_t source_index = 0;
            std::string_view name; // as cooked: the file's entry there must have it
            SourceTransform transform;
            TransformParts parts;
            FogEdit fog[8];
            size_t fog_count = 0;
            const char* folder = nullptr;                   // its folder, when it isn't the one its file has
            const world::PrefabInstance* variant = nullptr; // the placement, when it asks for another variant now
            const world::LocalLight* light = nullptr;       // its light, when it isn't as cooked
        };
        core::Array<Change> changes(saver_heap());
        core::Array<Refile> leaving(saver_heap());
        for (size_t i = 0; i < entities.size(); ++i) {
            const asset::CellEntity& entry = entities[i];
            const flecs::entity entity = by_index[i] != 0 ? ecs.entity(by_index[i]) : flecs::entity();
            const auto* now = entity.is_valid() ? entity.try_get<world::WorldTransform>() : nullptr;
            if (now == nullptr) {
                continue;
            }
            world::Transform local{.position = now->position, .rotation = now->rotation, .scale = now->scale};
            if (entry.parent != asset::CellEntity::kNone) {
                const flecs::entity parent =
                    by_index[entry.parent] != 0 ? ecs.entity(by_index[entry.parent]) : flecs::entity();
                const auto* parent_now = parent.is_valid() ? parent.try_get<world::WorldTransform>() : nullptr;
                if (parent_now == nullptr) {
                    continue; // its parent's gone: where it stands relative to it isn't known
                }
                local = world::local_of(*parent_now, *now);
            } else {
                // A root files by where it stands: its origin, divided by the cell's size, rounded down.
                const auto x = static_cast<int32_t>(std::floor(now->position.x / world::kCellSize));
                const auto z = static_cast<int32_t>(std::floor(now->position.z / world::kCellSize));
                if (x != cooked->coord_x || z != cooked->coord_z) {
                    leaving.push({.source_index = entry.source_index, .x = x, .z = z});
                }
            }
            Change change{.source_index = entry.source_index,
                          .name = cooked->name(cooked, entry),
                          .transform = {.position = local.position, .rotation = local.rotation, .scale = local.scale},
                          .parts = {.position = !same_position(local.position, entry.position),
                                    .rotation = !same_rotation(local.rotation, entry.rotation),
                                    .scale = !same_floats(&local.scale.x, entry.scale, 3)},
                          .fog = {},
                          .fog_count = 0};
            const auto* fog_now = entity.try_get<world::FogVolume>();
            if (fog_now != nullptr && entry.fog_volume < fogs.size()) {
                change.fog_count = fog_edits(fogs[entry.fog_volume], *fog_now, change.fog);
            }
            if (const auto* folder = entity.try_get<EditorFolder>();
                folder != nullptr && std::strcmp(folder->path, folder->filed) != 0) {
                change.folder = folder->path;
            }
            if (const auto* placed = entity.try_get<world::PrefabInstance>();
                placed != nullptr && placed->variant != entry.variant) {
                change.variant = placed;
            }
            const std::span<const asset::LightDesc> lights = cooked->lights.get(cooked);
            if (const auto* lit = entity.try_get<world::LocalLight>();
                lit != nullptr && (entry.light >= lights.size() || !same_light(lights[entry.light], *lit))) {
                change.light = lit;
            }
            const bool moved_here = change.parts.position || change.parts.rotation || change.parts.scale;
            // One leaving its cell is checked against the file too, though it may not have moved within it.
            const bool leaves = !leaving.empty() && leaving[leaving.size() - 1].source_index == entry.source_index;
            if (moved_here || change.fog_count > 0 || change.folder != nullptr || change.variant != nullptr ||
                change.light != nullptr || leaves) {
                changes.push(change);
            }
        }
        if (changes.empty()) {
            continue;
        }
        const CellSources::Cell* source_cell = sources.find(assets.asset(held->asset).id);
        const uint32_t file_index = source_cell != nullptr ? open_file(source_cell->path.c_str()) : UINT32_MAX;
        if (file_index == UINT32_MAX) {
            if (source_cell == nullptr) {
                DE_LOG_ERROR("editor", "cell [%d, %d] has edits, but its source wasn't found under src:/content",
                             cooked->coord_x, cooked->coord_z);
            }
            ++report.cells_failed;
            continue;
        }
        OpenFile& file = files[file_index];
        // The file must be the one the cell was cooked from: each entry where the cook had it.
        bool all_there = true;
        for (const Change& change : changes) {
            all_there = all_there && change.source_index < file.source.entity_count() &&
                        file.source.name(change.source_index) == change.name;
        }
        if (!all_there) {
            DE_LOG_ERROR("editor", "%s changed since it was cooked: cook it, then save again", file.path.c_str());
            ++report.cells_failed;
            continue;
        }
        core::Array<char> was(saver_heap());
        was.resize(file.source.view().size());
        std::memcpy(was.data(), file.source.view().data(), was.size());
        for (const Change& change : changes) {
            static_cast<void>(file.source.set_transform(change.source_index, change.transform, change.parts));
            for (size_t f = 0; f < change.fog_count; ++f) {
                static_cast<void>(file.source.set_fog_member(change.source_index, change.fog[f].key,
                                                             change.fog[f].value, change.fog[f].decimals));
            }
            if (change.folder != nullptr) {
                write_folder(file.source, change.source_index, change.folder);
            }
            if (change.variant != nullptr) {
                write_variant(file.source, change.source_index, assets, *change.variant);
            }
            if (change.light != nullptr) {
                write_light(file.source, change.source_index, *change.light);
            }
        }
        if (file.source.view() != std::string_view(was.data(), was.size())) {
            file.changed = true;
            report.entities_changed += static_cast<uint32_t>(changes.size());
        }
        for (Refile& refile : leaving) {
            refile.file = file_index;
            refiles.push(refile);
        }
    }

    // A world's file for the cell at x, z: the one open, the one it lists, or a new one in folder (made, not written
    // yet).
    const auto file_for = [&](uint32_t world_index, int32_t x, int32_t z, std::string_view folder) -> uint32_t {
        for (size_t i = 0; i < files.size(); ++i) {
            if (files[i].world == world_index && files[i].x == x && files[i].z == z &&
                (files[i].made || sources.cell_at(world_index, x, z) != nullptr)) {
                return static_cast<uint32_t>(i);
            }
        }
        if (const CellSources::Cell* there = sources.cell_at(world_index, x, z)) {
            return open_file(there->path.c_str());
        }
        platform::PathString path;
        for (int n = 1; n < 100; ++n) {
            char name[64];
            static_cast<void>(n == 1 ? std::snprintf(name, sizeof(name), "/cell_%d_%d.cell", x, z)
                                     : std::snprintf(name, sizeof(name), "/cell_%d_%d_%d.cell", x, z, n));
            if (!path.assign(folder) || !path.append(name)) {
                return UINT32_MAX;
            }
            bool taken = vfs.exists(path.c_str());
            for (const OpenFile& open : files) {
                taken = taken || open.path.view() == path.view();
            }
            if (!taken) {
                break;
            }
        }
        OpenFile& made = files.push(OpenFile(saver_heap()));
        made.path = path;
        new_cell_text(x, z, text);
        static_cast<void>(made.source.parse({text.data(), text.size()}));
        made.x = x;
        made.z = z;
        made.world = world_index;
        made.made = true;
        made.changed = true;
        ++report.cells_made;
        return static_cast<uint32_t>(files.size() - 1);
    };
    // An entry just put into a file: named apart from what's there (its new name returned), and its mesh, its effect
    // and its prefab found from the file's folder rather than from_folder.
    const auto settle = [&](OpenFile& to, uint32_t placed, const Name& name, std::string_view from_folder) {
        Name unique = name;
        if (!name.empty()) {
            for (int n = 2; n < 1000; ++n) {
                bool taken = false;
                for (uint32_t i = 0; i < to.source.entity_count() && !taken; ++i) {
                    taken = i != placed && to.source.name(i) == unique.view();
                }
                if (!taken) {
                    break;
                }
                char suffixed[160];
                static_cast<void>(std::snprintf(suffixed, sizeof(suffixed), "%s_%d", name.c_str(), n));
                static_cast<void>(unique.assign(suffixed));
            }
            if (unique.view() != name.view() || to.source.name(placed) != unique.view()) {
                char quoted[160];
                static_cast<void>(std::snprintf(quoted, sizeof(quoted), "\"%s\"", unique.c_str()));
                static_cast<void>(to.source.set_entity_member(placed, "name", quoted));
                if (unique.view() != name.view()) {
                    DE_LOG_INFO("editor", "%s is %s in %s: the name was taken there", name.c_str(), unique.c_str(),
                                to.path.c_str());
                }
            }
        }
        const std::string_view to_folder = folder_of(to.path.view());
        for (const std::string_view key :
             {std::string_view("mesh"), std::string_view("effect"), std::string_view("prefab")}) {
            const std::string_view named = to.source.string_member(placed, key);
            if (named.empty() || from_folder == to_folder) {
                continue;
            }
            char full[platform::kMaxPathLength];
            char relative[platform::kMaxPathLength + 2];
            size_t full_length = 0;
            size_t relative_length = 0;
            if (joined_path(from_folder, named, full, full_length) &&
                relative_path(to_folder, {full, full_length}, {relative + 1, sizeof(relative) - 2}, relative_length)) {
                relative[0] = '"';
                relative[relative_length + 1] = '"';
                static_cast<void>(to.source.set_entity_member(placed, key, {relative, relative_length + 2}));
            }
        }
        to.changed = true;
        to.received = true;
        return unique;
    };
    const uint32_t open_world = sources.world_index(world);

    // 2. What stands in another cell's square goes to that cell's file, with everything under it.
    core::Array<Moving> moving(saver_heap());
    for (const Refile& refile : refiles) {
        if (files[refile.file].world == UINT32_MAX) {
            ++report.outside_cell; // no world lists its file: nowhere to file it by
            continue;
        }
        // Its target: the world's cell there, else a file made for it next to the one it leaves.
        const uint32_t target =
            file_for(files[refile.file].world, refile.x, refile.z, folder_of(files[refile.file].path.view()));
        if (target == UINT32_MAX) {
            ++report.cells_failed;
            continue;
        }
        // The root, and what names it as a parent, and what names those, in the file's order.
        const CellSource& from = files[refile.file].source;
        core::Array<uint8_t> going(saver_heap());
        going.resize(from.entity_count(), 0);
        going[refile.source_index] = 1;
        for (bool grew = true; grew;) {
            grew = false;
            for (uint32_t i = 0; i < from.entity_count(); ++i) {
                if (going[i] != 0 || from.parent(i).empty()) {
                    continue;
                }
                for (uint32_t p = 0; p < from.entity_count(); ++p) {
                    if (going[p] != 0 && from.name(p) == from.parent(i)) {
                        going[i] = 1;
                        grew = true;
                        break;
                    }
                }
            }
        }
        for (uint32_t i = 0; i < from.entity_count(); ++i) {
            if (going[i] == 0) {
                continue;
            }
            Moving& entry = moving.push(Moving{});
            entry.from_file = refile.file;
            entry.from_index = i;
            entry.to_file = target;
            static_cast<void>(entry.name.assign(from.name(i)));
            static_cast<void>(entry.parent.assign(from.parent(i)));
            const std::string_view object = from.entity_text(i);
            entry.text.resize(object.size());
            std::memcpy(entry.text.data(), object.data(), object.size());
            files[refile.file].removed.push(i);
        }
        ++report.refiled;
    }

    // What was deleted: out of its file. What its file hangs on it becomes a root where it stands (it already was one,
    // in the world: a static child spawns as one).
    for (const MemberRemoval& removal : removed) {
        const CellSources::Cell* cell =
            open_world != UINT32_MAX ? sources.cell_at(open_world, removal.key.x, removal.key.z) : nullptr;
        const uint32_t file_index = cell != nullptr ? open_file(cell->path.c_str()) : UINT32_MAX;
        if (file_index == UINT32_MAX) {
            DE_LOG_ERROR("editor", "%.*s was deleted, but its cell [%d, %d]'s file wasn't found",
                         static_cast<int>(removal.name.size()), removal.name.data(), removal.key.x, removal.key.z);
            ++report.cells_failed;
            continue;
        }
        OpenFile& file = files[file_index];
        const uint32_t index = removal.key.source_index;
        if (index >= file.source.entity_count() || file.source.name(index) != removal.name) {
            DE_LOG_ERROR("editor", "%s changed since it was cooked: cook it, then save again", file.path.c_str());
            ++report.cells_failed;
            continue;
        }
        if (std::ranges::find(file.removed, index) != file.removed.end()) {
            continue;
        }
        file.removed.push(index);
        ++report.removed;
        if (removal.name.empty()) {
            continue;
        }
        for (uint32_t i = 0; i < file.source.entity_count(); ++i) {
            if (file.source.parent(i) != removal.name || std::ranges::find(file.removed, i) != file.removed.end()) {
                continue;
            }
            const flecs::entity_t* child = by_key.find(pack_key({.x = file.x, .z = file.z, .source_index = i}));
            const auto* where = child != nullptr && ecs.is_alive(*child)
                                    ? ecs.entity(*child).try_get<world::WorldTransform>()
                                    : nullptr;
            if (where != nullptr) {
                static_cast<void>(file.source.remove_entity_member(i, "parent"));
                static_cast<void>(file.source.set_transform(
                    i, {.position = where->position, .rotation = where->rotation, .scale = where->scale}));
            }
        }
    }

    // Out of the files they leave, the last first, so the places of the others hold while they go.
    for (OpenFile& file : files) {
        if (file.removed.empty()) {
            continue;
        }
        std::ranges::sort(file.removed, std::greater<>());
        for (const uint32_t index : file.removed) {
            static_cast<void>(file.source.remove_entity(index));
        }
        file.changed = true;
        // Those after them moved up.
        if (moves != nullptr) {
            const auto total = static_cast<uint32_t>(file.source.entity_count() + file.removed.size());
            for (uint32_t index = 0; index < total; ++index) {
                const auto gone_before = static_cast<uint32_t>(
                    std::ranges::count_if(file.removed, [index](uint32_t removed_at) { return removed_at < index; }));
                const bool gone = std::ranges::find(file.removed, index) != file.removed.end();
                if (!gone && gone_before > 0) {
                    moves->push({.from = {.x = file.x, .z = file.z, .source_index = index},
                                 .to = {.x = file.x, .z = file.z, .source_index = index - gone_before}});
                }
            }
        }
    }
    // Into the files they go to, named apart from what's there, their meshes found from the new folder.
    for (size_t m = 0; m < moving.size(); ++m) {
        Moving& entry = moving[m];
        const uint32_t placed = files[entry.to_file].source.add_entity({entry.text.data(), entry.text.size()});
        const Name unique =
            settle(files[entry.to_file], placed, entry.name, folder_of(files[entry.from_file].path.view()));
        if (unique.view() != entry.name.view()) {
            // What names it as a parent, coming with it, follows.
            for (size_t c = m + 1; c < moving.size(); ++c) {
                if (moving[c].to_file == entry.to_file && moving[c].parent.view() == entry.name.view()) {
                    static_cast<void>(moving[c].parent.assign(unique.view()));
                    moving[c].parent_renamed = true;
                }
            }
        }
        OpenFile& to = files[entry.to_file];
        if (entry.parent_renamed) {
            char quoted[160];
            static_cast<void>(std::snprintf(quoted, sizeof(quoted), "\"%s\"", entry.parent.c_str()));
            static_cast<void>(to.source.set_entity_member(placed, "parent", quoted));
        }
        if (moves != nullptr) {
            const OpenFile& from = files[entry.from_file];
            moves->push({.from = {.x = from.x, .z = from.z, .source_index = entry.from_index},
                         .to = {.x = to.x, .z = to.z, .source_index = placed}});
        }
    }

    // What the editor made: into the file of the cell it stands in, in the world open (a new file beside the world's
    // if that cell has none).
    ecs.each([&](flecs::entity entity, const EditorEntry& made, const world::WorldTransform& where) {
        if (made.saved) {
            return; // in a file already: its cell comes back cooked with it
        }
        if (open_world == UINT32_MAX) {
            DE_LOG_ERROR("editor", "%s can't be saved: no world under src:/content is the one open", made.name);
            ++report.cells_failed;
            return;
        }
        const auto x = static_cast<int32_t>(std::floor(where.position.x / world::kCellSize));
        const auto z = static_cast<int32_t>(std::floor(where.position.z / world::kCellSize));
        const uint32_t target = file_for(open_world, x, z, folder_of(sources.world(open_world).path.view()));
        if (target == UINT32_MAX) {
            ++report.cells_failed;
            return;
        }
        const uint32_t placed = files[target].source.add_entity(made.object);
        static_cast<void>(files[target].source.set_transform(
            placed, {.position = where.position, .rotation = where.rotation, .scale = where.scale}));
        Name name;
        static_cast<void>(name.assign(made.name));
        static_cast<void>(settle(files[target], placed, name, made.folder));
        if (const auto* folder = entity.try_get<EditorFolder>()) {
            write_folder(files[target].source, placed, folder->path);
        }
        if (const auto* instance = entity.try_get<world::PrefabInstance>()) {
            write_variant(files[target].source, placed, assets, *instance);
        }
        if (const auto* lit = entity.try_get<world::LocalLight>()) {
            write_light(files[target].source, placed, *lit); // as it is now, whatever its entry was made with
        }
        ++report.added;
        ++report.entities_changed;
        if (placed_out != nullptr) {
            placed_out->push(
                {.entity = entity.id(), .key = {.x = files[target].x, .z = files[target].z, .source_index = placed}});
        }
    });

    // 3. Every file that changed: first those things moved into, then the rest. If one of the first can't be written,
    // the files things left aren't either, so nothing is in neither.
    bool receiving_failed = false;
    for (int pass = 0; pass < 2; ++pass) {
        for (const OpenFile& file : files) {
            if (!file.changed || file.received != (pass == 0)) {
                continue;
            }
            if (pass == 1 && receiving_failed && !file.removed.empty()) {
                DE_LOG_ERROR("editor", "%s isn't written: what moves out of it couldn't be written where it goes",
                             file.path.c_str());
                ++report.cells_failed;
                continue;
            }
            if (const core::Result<void> stored = vfs.write_file(file.path.c_str(), std::as_bytes(file.source.text()));
                !stored) {
                DE_LOG_ERROR("editor", "can't write %s: %s", file.path.c_str(), stored.error().message);
                ++report.cells_failed;
                receiving_failed = receiving_failed || pass == 0;
                continue;
            }
            DE_LOG_INFO("editor", "saved %s%s", file.path.c_str(), file.made ? " (a new cell)" : "");
            ++report.files_written;
            if (written != nullptr) {
                written->push(file.path);
            }
        }
    }
    core::Array<char> world_text(saver_heap());
    core::Array<char> edited(saver_heap());
    for (const OpenFile& file : files) {
        if (!file.made || file.world == UINT32_MAX) {
            continue;
        }
        const CellSources::World& owner = sources.world(file.world);
        char listed[platform::kMaxPathLength];
        size_t length = 0;
        if (!read_text(vfs, owner.path.c_str(), world_text) ||
            !relative_path(folder_of(owner.path.view()), file.path.view(), listed, length) ||
            !add_world_cell({world_text.data(), world_text.size()}, {listed, length}, edited) ||
            !vfs.write_file(owner.path.c_str(), std::as_bytes(edited.span()))) {
            DE_LOG_ERROR("editor", "can't add %s to %s's cells: add it by hand", file.path.c_str(), owner.path.c_str());
            ++report.cells_failed;
            continue;
        }
        DE_LOG_INFO("editor", "added %.*s to %s", static_cast<int>(length), listed, owner.path.c_str());
        ++report.files_written;
        if (written != nullptr) {
            written->push(owner.path);
        }
    }
    return report;
}

bool cook_report_has_error(std::string_view report, std::string_view source) {
    // The report names sources from src:/ ("content/a.cell"); each diagnostic writes its severity, then its path.
    constexpr std::string_view kSrc = "src:/";
    const std::string_view path = source.starts_with(kSrc) ? source.substr(kSrc.size()) : source;
    constexpr std::string_view kError = R"("severity": "error")";
    constexpr std::string_view kPath = R"("path": ")";
    for (size_t at = report.find(kError); at != std::string_view::npos; at = report.find(kError, at + 1)) {
        const size_t end = report.find('}', at);
        const size_t key = report.find(kPath, at);
        if (key == std::string_view::npos || key > end) {
            continue;
        }
        const size_t begin = key + kPath.size();
        const size_t close = report.find('"', begin);
        if (close != std::string_view::npos && report.substr(begin, close - begin) == path) {
            return true;
        }
    }
    return false;
}

// ---- MemberTracker ----

uint64_t MemberTracker::pack(const MemberKey& key) {
    return pack_key(key);
}

void MemberTracker::note(const flecs::world& ecs, const asset::AssetRegistry& assets, flecs::entity_t id) {
    if (id == 0 || !ecs.is_alive(id)) {
        return;
    }
    const flecs::entity entity = ecs.entity(id);
    const auto* member = entity.try_get<world::CellMember>();
    const flecs::entity cell = member != nullptr ? entity.target<world::InCell>() : flecs::entity();
    const auto* held = cell.is_valid() ? cell.try_get<world::Cell>() : nullptr;
    const auto* cooked = held != nullptr ? assets.get<asset::CellAsset>(held->asset) : nullptr;
    if (cooked == nullptr) {
        return;
    }
    const std::span<const asset::CellEntity> entities = cooked->entities.get(cooked);
    if (member->index < entities.size()) {
        keys_.set(id, {.key = {.x = cooked->coord_x,
                               .z = cooked->coord_z,
                               .source_index = entities[member->index].source_index}});
    }
}

void MemberTracker::apply(std::span<const MemberMove> moves) {
    for (auto [entity, noted] : keys_) {
        if (noted.replaced) {
            continue; // its key is where the save put it
        }
        for (const MemberMove& move : moves) {
            if (move.from == noted.key) {
                noted.key = move.to;
                break;
            }
        }
    }
}

void MemberTracker::note_replaced(const flecs::world& ecs, flecs::entity_t entity, const MemberKey& key) {
    // Its cell as it is now: the member comes with the cell spawned after this.
    uint32_t revision = 0;
    ecs.each([&](const world::Cell& cell, const world::CellContents& contents) {
        if (cell.coord.x == key.x && cell.coord.z == key.z) {
            revision = contents.revision;
        }
    });
    keys_.set(entity, {.key = key, .replaced = true, .revision = revision});
}

void MemberTracker::follow(const flecs::world& ecs, const asset::AssetRegistry& assets,
                           core::FunctionRef<void(flecs::entity_t from, flecs::entity_t to)> moved) {
    // The cells spawned since the last call (again, or for the first time), by their place.
    spawned_.clear();
    ecs.each([&](flecs::entity cell, const world::Cell& held, const world::CellContents& contents) {
        uint32_t* known = revisions_.find(cell.id());
        const bool first = known == nullptr;
        if (first) {
            revisions_.set(cell.id(), contents.revision);
        } else if (*known != contents.revision) {
            *known = contents.revision;
        } else {
            return;
        }
        if (contents.revision != 0) {
            spawned_.set(pack_key({.x = held.coord.x, .z = held.coord.z, .source_index = 0}), contents.revision);
        }
    });
    if (spawned_.empty() || keys_.empty()) {
        return;
    }
    // Their members by key, then each noted entity that's gone (or saved, made in the editor) finds its own there.
    members_.clear();
    ecs.each([&](flecs::entity entity, const world::CellMember& member) {
        const flecs::entity cell = entity.target<world::InCell>();
        const auto* held = cell.is_valid() ? cell.try_get<world::Cell>() : nullptr;
        const auto* cooked = held != nullptr ? assets.get<asset::CellAsset>(held->asset) : nullptr;
        if (cooked == nullptr || member.index >= cooked->entities.count ||
            !spawned_.contains(pack_key({.x = cooked->coord_x, .z = cooked->coord_z, .source_index = 0}))) {
            return;
        }
        members_.set(pack({.x = cooked->coord_x,
                           .z = cooked->coord_z,
                           .source_index = cooked->entities.get(cooked)[member.index].source_index}),
                     entity.id());
    });
    moved_.clear();
    for (auto [entity, noted] : keys_) {
        if (ecs.is_alive(entity) && !noted.replaced) {
            continue;
        }
        if (noted.replaced) {
            // Only by its cell spawned after the save wrote it in.
            const uint32_t* revision = spawned_.find(pack_key({.x = noted.key.x, .z = noted.key.z, .source_index = 0}));
            if (revision == nullptr || *revision == noted.revision) {
                continue;
            }
        }
        if (const flecs::entity_t* now = members_.find(pack(noted.key)); now != nullptr) {
            moved_.push({.from = entity, .to = *now, .noted = {.key = noted.key}});
        }
    }
    for (const Moved& move : moved_) {
        static_cast<void>(keys_.remove(move.from));
        keys_.set(move.to, move.noted);
        moved(move.from, move.to);
    }
}

} // namespace de::editor
