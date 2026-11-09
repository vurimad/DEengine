#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/function_ref.h"
#include "core/hash_map.h"
#include "editor/entity_record.h"

#include <flecs.h>

#include <cstddef>
#include <cstdint>
#include <span>

namespace de::editor {

// The editor's changes, as commands that can be undone and redone (docs/design.md, Editor & tooling: the editor never
// changes the world directly): component.set (a component's new value on an entity, as its bytes: a component of plain
// data), entity.create (one the editor made: undo takes it away, redo makes it again) and entity.delete (the other way
// round). An entity made again is a new entity: every command naming the old one names the new one from then on, and
// last_made() says which it is. The command API (a text form, Lua, the agents' socket) grows from here.
//
//     commands.set_component(entity, transform_id, std::as_bytes(std::span(&edited, 1)), merge_key);
//     ...
//     commands.undo(ecs);   // Ctrl+Z
class CommandStack {
public:
    static constexpr size_t kMaxCommands = 512; // past this the oldest are forgotten

    explicit CommandStack(core::Allocator& allocator)
        : allocator_(&allocator), commands_(allocator), saved_(allocator) {}

    // Sets the entity's component to value, as a command: what it had is kept for undo, and anything undone is
    // forgotten. Sets with the same nonzero merge key in a row (one drag of a slider) make one command, from the first
    // value before to the last; seal() ends the run. False (nothing done) if the entity is gone, doesn't have the
    // component, or value isn't the component's size.
    bool set_component(flecs::entity entity, flecs::id_t component, std::span<const std::byte> value,
                       uint64_t merge_key = 0);
    // Ends a run of merged sets: the next set is a command of its own.
    void seal() { open_merge_key_ = 0; }

    // Undo and redo make and take away entities through maker (the editor); without one there are no entity commands.
    void set_maker(EntityMaker* maker) { maker_ = maker; }
    // The entity the editor just made, as a command (what it is is taken from it now).
    void created(flecs::entity entity);
    // Takes the entity away, as a command, with everything attached to it (what each is is taken from it first; undo
    // makes them all again, attached as they were). False if it's gone, or there's no maker.
    bool remove(flecs::entity entity);
    // The entity the last undo or redo made (0: none): the one to select.
    flecs::entity_t last_made() const { return last_made_; }
    // Each record of a cell's member that's taken away now (deleted, or made and undone after a save filed it), whose
    // cell is as it was then: what a save takes out of the files.
    void each_taken_member(const flecs::world& ecs, core::FunctionRef<void(const EntityRecord& record)> visit) const;

    // Puts back what the last command changed, or does again what the last undo put back. False if there's nothing to
    // undo or redo, or its entity is gone (the command is dropped then).
    bool undo(const flecs::world& ecs);
    bool redo(const flecs::world& ecs);
    bool can_undo() const { return done_ > 0; }
    bool can_redo() const { return done_ < commands_.size(); }
    // What the next undo or redo does ("de.Transform", "Create", "Delete"), for the menu; null if there's none.
    const char* undo_name(const flecs::world& ecs) const;
    const char* redo_name(const flecs::world& ecs) const;

    // What the component held before its first edit on the entity: the saved value, which the reset arrow puts back
    // (component.size bytes); null if it hasn't been edited.
    const std::byte* saved_value(flecs::entity_t entity, flecs::id_t component) const;

    // Forgets every command and saved value (another world was opened: their entities are gone).
    void clear();
    // Forgets the saved values only: a save wrote the world as it is, so that's what's saved now.
    void forget_saved_values() { saved_.clear(); }
    // Every set, undo and redo counts one: the editor compares it with the count at its last save.
    uint64_t changes() const { return changes_; }
    // The commands and saved values of one entity now go to another: the same thing spawned again (its cell was cooked
    // again after a save, and hot reload spawned its members anew).
    void remap(flecs::entity_t from, flecs::entity_t to);
    // The entity of each command, in order, into out.
    void entities(core::Array<flecs::entity_t>& out) const;
    size_t size() const { return commands_.size(); }

private:
    enum class Kind : uint8_t { Set, Create, Remove };
    struct Command {
        Kind kind = Kind::Set;
        flecs::entity_t entity = 0;
        flecs::id_t component = 0;
        uint64_t merge_key = 0;
        core::Array<std::byte> before;
        core::Array<std::byte> after;
        core::Array<EntityRecord> record; // Create and Remove: what the entity is, then what's attached to it
    };
    void push(Command&& command);
    // Create's undo and Remove's do: the entity goes, what it is kept. Remove's undo and Create's redo: made again.
    void take_away(Command& command, const flecs::world& ecs);
    void make_again(Command& command);
    const char* name_of(const Command& command, const flecs::world& ecs) const;

    core::Allocator* allocator_;
    core::Array<Command> commands_;
    core::HashMap<uint64_t, core::Array<std::byte>> saved_; // by entity and component
    size_t done_ = 0;                                       // commands_[0, done_) are applied; the rest were undone
    uint64_t open_merge_key_ = 0;
    uint64_t changes_ = 0; // the run the last command takes more sets into; 0: none
    EntityMaker* maker_ = nullptr;
    flecs::entity_t last_made_ = 0;
};

// Writes value as the entity's component, as the commands do: a Transform is placed (world::place), so the entity and
// what's attached to it move at once, without sliding there over a tick; anything else is set (its hooks and
// observers run). value is the component's size.
void apply_component(flecs::entity entity, flecs::id_t component, std::span<const std::byte> value);

} // namespace de::editor
