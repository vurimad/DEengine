#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/function_ref.h"

#include <flecs.h>

#include <cstddef>
#include <span>
#include <string_view>

namespace de::asset {
class AssetRegistry;
}

namespace de::editor {

// What the hierarchy panel lists, read from the world: its cells, the members of each, what's in no cell (the camera,
// dropped rocks), and what's attached to each of those (flecs::Parent or ChildOf). Read again when asked, not every
// frame: a world has thousands of entities.
class Outline {
public:
    struct Item {
        flecs::entity_t parent = 0; // what it's attached to; 0: a root
        flecs::entity_t cell = 0;   // the cell it's in; 0: none
        flecs::entity_t entity = 0;
    };

    explicit Outline(core::Allocator& allocator) : cells_(allocator), items_(allocator) {}

    // Reads the world again: the cells (Cell) and everything placed (Transform).
    void rebuild(const flecs::world& ecs);

    // The cells, in the order of their coordinates (rows of z, then x).
    std::span<const flecs::entity_t> cells() const { return cells_.span(); }
    // A cell's members that are attached to nothing; cell 0: what's in no cell and attached to nothing.
    std::span<const Item> roots(flecs::entity_t cell) const { return range(0, cell, false); }
    // Everything attached to nothing, in a cell or not.
    std::span<const Item> roots() const { return range(0, 0, true); }
    // What's attached to an entity.
    std::span<const Item> children(flecs::entity_t parent) const { return range(parent, 0, true); }
    // Everything placed, cells not included.
    std::span<const Item> items() const { return items_.span(); }

private:
    std::span<const Item> range(flecs::entity_t parent, flecs::entity_t cell, bool any_cell) const;

    core::Array<flecs::entity_t> cells_;
    core::Array<Item> items_; // by parent, then cell, then entity
};

// The entity's name for people, into out (terminated, cut short if it doesn't fit): its flecs name, its name in its
// cell's file, "cell [x, z]" for a cell, or what it is and its number ("Mesh 1234"); "(gone)" if it's no longer there.
void entity_label(const asset::AssetRegistry& assets, flecs::entity entity, std::span<char> out);

// A name that isn't taken, from base, into out (terminated): base itself if it's free; else a trailing number counts
// up, keeping its width ("boulder_1" to "boulder_5" if 2 to 4 are taken, "rock_01" to "rock_02"), and a name without
// one gets "_2", "_3"...
void free_name(std::string_view base, core::FunctionRef<bool(std::string_view name)> taken, std::span<char> out);

// What the entity is, for people: "Cell", "Camera", "Fog volume", "Water", "Body", "Mesh" or "Entity".
const char* entity_kind(flecs::entity entity);

// Whether text holds part (ASCII letters in either case); an empty part is in everything.
bool contains_ignoring_case(std::string_view text, std::string_view part);

} // namespace de::editor
