#pragma once

#include "core/fixed_string.h"

#include <flecs.h>

#include <cstdint>
#include <string_view>

namespace de::editor {

// What the editor has chosen: an entity in the world (the hierarchy) or a source file (the resource browser). The
// properties panel shows whichever was chosen last.
struct Selection {
    enum class Kind : uint8_t { None, Entity, File };

    Kind kind = Kind::None;
    flecs::entity_t entity = 0;
    core::FixedString<255> file; // "src:/content/terrain/wc.world"
    bool frame_entity = false;   // asked this frame: the camera goes to look at the entity

    void select_entity(flecs::entity_t chosen) {
        kind = chosen != 0 ? Kind::Entity : Kind::None;
        entity = chosen;
    }
    void select_file(std::string_view path) {
        kind = Kind::File;
        file.assign(path);
    }
};

} // namespace de::editor
