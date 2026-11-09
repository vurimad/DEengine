#pragma once

#include "asset/asset_id.h"
#include "editor/cell_saver.h"
#include "world/render_extract.h"
#include "world/transform.h"

#include <flecs.h>

#include <cstdint>

namespace de::editor {

// On an entity the editor made (placed from the resources, duplicated, or a cell's member taken away and put back
// after a save took it out of its file): the entry it's saved as in a cell's file, at the next save, in the cell its
// position falls in. Until its cell comes back cooked with it, then the member spawned from that entry takes its place.
struct EditorEntry {
    char object[1024] = {}; // "{...}": its keys but where it stands (the save writes that)
    char folder[256] = {};  // the src:/ folder its paths are relative to ("src:/content/props/rock")
    char name[128] = {};    // what it shows as until the save names it in its cell
    bool saved = false;     // written into a cell's file, waiting for the cell to come back cooked
};

// What an entity is, to make it again (undo, redo): taken from it before it goes.
struct EntityRecord {
    world::Transform transform; // in the world: it comes back a root, where it stood
    asset::AssetId mesh;        // invalid: none
    asset::AssetId collision;
    bool has_fog = false;
    world::FogVolume fog;
    bool is_static = true;
    // A cell's member: its cell, which of the cooked cell's entities it is, the cell's revision then, where it is in
    // the cell's file and its name there (the save checks the file still has it).
    flecs::entity_t cell = 0;
    uint32_t member_index = 0;
    uint32_t revision = 0;
    MemberKey key;
    char cooked_name[128] = {};
    // As an entry of its own: for one the editor made, or a member put back once a save took it out of its file.
    EditorEntry entry;
    bool has_entry = false;
    // Among what a command takes away together (the entity, then what's attached to it, each after what it hangs on):
    // the entity it was, and the one before it in the list it's attached to (UINT32_MAX: none, the first).
    flecs::entity_t was = 0;
    uint32_t attached_to = UINT32_MAX;
};

// Makes and takes away entities for the commands (undo, redo): the editor, which holds what they draw with.
class EntityMaker {
public:
    EntityMaker() = default;
    virtual ~EntityMaker() = default;
    EntityMaker(const EntityMaker&) = delete;
    EntityMaker& operator=(const EntityMaker&) = delete;
    EntityMaker(EntityMaker&&) = delete;
    EntityMaker& operator=(EntityMaker&&) = delete;

    // A new entity as the record says: a member of its cell again if the cell is as it was, else one the editor made.
    virtual flecs::entity make(const EntityRecord& record) = 0;
    // What the entity is now (before it's taken away).
    virtual void record(flecs::entity entity, EntityRecord& out) = 0;
    virtual void unmake(flecs::entity entity) = 0;
};

} // namespace de::editor
