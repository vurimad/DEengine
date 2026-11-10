#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"

#include <flecs.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace de::editor {

// What the editor has chosen: entities in the world (the Playground, the hierarchy), the world's terrain (the
// hierarchy: it's outlined, and can't be moved), or a source file (the resource browser). The properties panel shows
// whichever was chosen last. Several entities can be chosen (Ctrl toggles one, Shift adds); the one chosen last is the
// primary: what the properties show and where the handles sit, and what an edit of all of them goes by.
struct Selection {
    enum class Kind : uint8_t { None, Entity, Terrain, File };

    Kind kind = Kind::None;
    flecs::entity_t entity = 0; // the primary: the one chosen last
    core::Array<flecs::entity_t> entities{core::heap_allocator(core::MemoryTag::Editor)}; // every one, it among them
    core::FixedString<255> file; // "src:/content/terrain/wc.world"
    bool frame_entity = false;   // asked this frame: the camera goes to look at the entity (or the terrain)

    // Only this one (none for 0).
    void select_entity(flecs::entity_t chosen) {
        entities.clear();
        if (chosen != 0) {
            entities.push(chosen);
        }
        kind = chosen != 0 ? Kind::Entity : Kind::None;
        entity = chosen;
    }
    // This one too, and it's the primary now.
    void add_entity(flecs::entity_t chosen) {
        if (chosen == 0) {
            return;
        }
        if (kind != Kind::Entity) {
            entities.clear();
        }
        if (!contains(chosen)) {
            entities.push(chosen);
        }
        kind = Kind::Entity;
        entity = chosen;
    }
    // In if it was out (the primary then), out if it was in.
    void toggle_entity(flecs::entity_t chosen) {
        if (kind == Kind::Entity && contains(chosen)) {
            remove_entity(chosen);
        } else {
            add_entity(chosen);
        }
    }
    // Out (gone, or taken away): the primary becomes the one chosen last of those left.
    void remove_entity(flecs::entity_t gone) {
        if (const auto* at = std::ranges::find(entities, gone); at != entities.end()) {
            entities.remove_ordered(static_cast<size_t>(at - entities.begin()));
        }
        if (entity == gone) {
            entity = entities.empty() ? 0 : entities.back();
        }
        if (kind == Kind::Entity && entities.empty()) {
            kind = Kind::None;
        }
    }
    // The same thing as another entity now (made again, spawned again).
    void replace_entity(flecs::entity_t from, flecs::entity_t to) {
        for (flecs::entity_t& chosen : entities) {
            if (chosen == from) {
                chosen = to;
            }
        }
        if (entity == from) {
            entity = to;
        }
    }
    bool contains(flecs::entity_t chosen) const { return std::ranges::find(entities, chosen) != entities.end(); }
    // Whether an entity is chosen now (not just in the list behind a chosen file).
    bool has_entity(flecs::entity_t chosen) const { return kind == Kind::Entity && contains(chosen); }

    // The terrain, alone.
    void select_terrain() {
        entities.clear();
        entity = 0;
        kind = Kind::Terrain;
    }
    void select_file(std::string_view path) {
        kind = Kind::File;
        file.assign(path);
    }
};

} // namespace de::editor
