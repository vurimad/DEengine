#include "editor/outline.h"

#include "asset/cell_format.h"
#include "asset/registry.h"
#include "core/profile.h"
#include "editor/entity_record.h"
#include "physics/physics.h"
#include "world/cell.h"
#include "world/prefab.h"
#include "world/render_extract.h"
#include "world/transform.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace de::editor {

namespace {

bool item_before(const Outline::Item& a, const Outline::Item& b) {
    if (a.parent != b.parent) {
        return a.parent < b.parent;
    }
    if (a.cell != b.cell) {
        return a.cell < b.cell;
    }
    return a.entity < b.entity;
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

} // namespace

void Outline::rebuild(const flecs::world& ecs) {
    DE_PROFILE_ZONE("Outline::rebuild");
    cells_.clear();
    ecs.each([this](flecs::entity cell, const world::Cell&) { cells_.push(cell.id()); });
    std::ranges::sort(cells_, [&ecs](flecs::entity_t a, flecs::entity_t b) {
        const world::CellCoord& at = ecs.entity(a).get<world::Cell>().coord;
        const world::CellCoord& bt = ecs.entity(b).get<world::Cell>().coord;
        return at.z != bt.z ? at.z < bt.z : at.x < bt.x;
    });

    items_.clear();
    ecs.each([this](flecs::entity entity, const world::Transform&) {
        if (entity.has<world::PrefabPart>()) {
            return; // its placement stands for it: a placed prefab is one thing
        }
        flecs::entity_t parent = 0;
        if (const auto* attached = entity.try_get<flecs::Parent>()) {
            parent = attached->value;
        } else {
            parent = entity.parent().id();
        }
        items_.push({.parent = parent, .cell = entity.target<world::InCell>().id(), .entity = entity.id()});
    });
    std::ranges::sort(items_, item_before);
}

std::span<const Outline::Item> Outline::range(flecs::entity_t parent, flecs::entity_t cell, bool any_cell) const {
    const Item low{.parent = parent, .cell = any_cell ? 0 : cell, .entity = 0};
    const Item high{.parent = parent, .cell = any_cell ? UINT64_MAX : cell, .entity = UINT64_MAX};
    const Item* first = std::ranges::lower_bound(items_, low, item_before);
    const Item* last = std::ranges::upper_bound(first, items_.end(), high, item_before);
    return {first, static_cast<size_t>(last - first)};
}

void free_name(std::string_view base, core::FunctionRef<bool(std::string_view name)> taken, std::span<char> out) {
    if (out.empty()) {
        return;
    }
    const auto put = [&out](std::string_view name) {
        const size_t length = std::min(name.size(), out.size() - 1);
        std::memcpy(out.data(), name.data(), length);
        out[length] = 0;
    };
    if (!taken(base)) {
        put(base);
        return;
    }
    // "stem_digits": the number counts up, as wide as it was; otherwise "_2" on.
    size_t digits = base.size();
    while (digits > 0 && base[digits - 1] >= '0' && base[digits - 1] <= '9') {
        --digits;
    }
    const bool numbered = digits < base.size() && digits > 0 && base[digits - 1] == '_';
    const std::string_view stem = numbered ? base.substr(0, digits - 1) : base;
    const int width = numbered ? static_cast<int>(base.size() - digits) : 1;
    int number = 1;
    if (numbered) {
        static_cast<void>(std::from_chars(base.data() + digits, base.data() + base.size(), number));
    }
    char candidate[160];
    for (int n = number + 1; n < number + 100000; ++n) {
        const int length = std::snprintf(candidate, sizeof(candidate), "%.*s_%0*d", static_cast<int>(stem.size()),
                                         stem.data(), width, n);
        if (length > 0 && !taken({candidate, static_cast<size_t>(length)})) {
            put({candidate, static_cast<size_t>(length)});
            return;
        }
    }
    put(base);
}

void entity_label(const asset::AssetRegistry& assets, flecs::entity entity, std::span<char> out) {
    if (out.empty()) {
        return;
    }
    // Gone (its cell spawned again since a panel last looked): said so, not asked of.
    if (!entity.is_valid() || !entity.is_alive()) {
        static_cast<void>(std::snprintf(out.data(), out.size(), "(gone)"));
        return;
    }
    // Cut short if it doesn't fit: a label is only for people.
    if (const char* name = entity.name().c_str(); name != nullptr && name[0] != '\0') {
        static_cast<void>(std::snprintf(out.data(), out.size(), "%s", name));
        return;
    }
    if (const auto* cell = entity.try_get<world::Cell>()) {
        static_cast<void>(std::snprintf(out.data(), out.size(), "cell [%d, %d]", cell->coord.x, cell->coord.z));
        return;
    }
    // One the editor made: the name it was given.
    if (const auto* made = entity.try_get<EditorEntry>(); made != nullptr && made->name[0] != 0) {
        static_cast<void>(std::snprintf(out.data(), out.size(), "%s", made->name));
        return;
    }
    // A cell's member: its name in the cell's file.
    const auto* member = entity.try_get<world::CellMember>();
    const flecs::entity cell = entity.target<world::InCell>();
    const auto* holder = cell.is_valid() ? cell.try_get<world::Cell>() : nullptr;
    if (member != nullptr && holder != nullptr) {
        if (const auto* asset = assets.get<asset::CellAsset>(holder->asset)) {
            const std::span<const std::byte> file = assets.file(holder->asset);
            const std::span<const asset::CellEntity> entities = asset->entities.get(file.data());
            if (member->index < entities.size()) {
                const std::string_view name = asset->name(file.data(), entities[member->index]);
                if (!name.empty()) {
                    static_cast<void>(
                        std::snprintf(out.data(), out.size(), "%.*s", static_cast<int>(name.size()), name.data()));
                    return;
                }
            }
        }
    }
    static_cast<void>(
        std::snprintf(out.data(), out.size(), "%s %u", entity_kind(entity), static_cast<uint32_t>(entity.id())));
}

const char* entity_kind(flecs::entity entity) {
    if (entity.has<world::Cell>()) {
        return "Cell";
    }
    if (entity.has<world::Camera>()) {
        return "Camera";
    }
    if (entity.has<world::PrefabInstance>()) {
        return "Prefab";
    }
    if (entity.has<world::FogVolume>()) {
        return "Fog volume";
    }
    if (entity.has<world::WaterBody>()) {
        return "Water";
    }
    if (entity.has<physics::RigidBody>()) {
        return "Body";
    }
    if (entity.has<world::MeshInstance>()) {
        return "Mesh";
    }
    if (entity.has<world::EffectInstance>()) {
        return "Effect";
    }
    return "Entity";
}

bool contains_ignoring_case(std::string_view text, std::string_view part) {
    if (part.empty()) {
        return true;
    }
    if (part.size() > text.size()) {
        return false;
    }
    for (size_t start = 0; start + part.size() <= text.size(); ++start) {
        size_t i = 0;
        while (i < part.size() && lower(text[start + i]) == lower(part[i])) {
            ++i;
        }
        if (i == part.size()) {
            return true;
        }
    }
    return false;
}

} // namespace de::editor
