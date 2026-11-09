#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/fixed_string.h"
#include "core/hash_map.h"
#include "editor/outline.h"
#include "editor/selection.h"

#include <flecs.h>

#include <cstdint>

namespace de::app {
struct Engine;
}

namespace de::editor {

// The hierarchy: what's placed in the world, by name, each with what's attached to it under it. Cells don't show here:
// they're how the world is stored and streamed (the cell map shows them). Drawn as the user's tree: a search field,
// rows on the navigation surface selected whole, a disclosure triangle for what has children; what each is shows on
// hover; the number of rows below. A click selects; a double-click opens or closes a branch, or frames the camera on a
// leaf (F frames the selection). Up and down move the selection, right opens or goes in, left closes or goes out. A
// selection made elsewhere opens the tree down to it. It reads the world four times a second, and when a branch opens
// or closes.
class HierarchyPanel {
public:
    static constexpr const char* kWindow = "Hierarchy";

    explicit HierarchyPanel(core::Allocator& allocator)
        : outline_(allocator), rows_(allocator), roots_(allocator), open_(allocator) {}

    // hidden: an entity left out (the editor's own camera).
    void draw(const app::Engine& engine, Selection& selection, flecs::entity_t hidden);
    // The world changed under it (another world opened): read it again now, every branch closed.
    void reset();
    // What the status bar shows while the panel has focus.
    const char* status() const { return status_; }
    bool focused() const { return focused_; }

private:
    struct Row {
        flecs::entity_t entity = 0;
        uint32_t depth = 0;
        bool has_children = false;
    };
    struct Root {
        flecs::entity_t entity = 0;
        core::FixedString<63> name;
    };

    void build_rows(const app::Engine& engine, flecs::entity_t hidden);
    void add_items(std::span<const Outline::Item> items, uint32_t depth);
    bool is_open(const Row& row) const { return open_.contains(row.entity); }
    void set_open(const Row& row, bool open);
    void keyboard(Selection& selection);
    // Shift-click: the rows from one to the other chosen, as they're listed.
    void choose_range(Selection& selection, flecs::entity_t from, flecs::entity_t to) const;
    // Opens the branches down to an entity chosen elsewhere (a link, --select), and scrolls to it.
    void reveal(const flecs::world& ecs, flecs::entity_t entity);

    Outline outline_;
    core::Array<Row> rows_;
    core::Array<Root> roots_;                      // what's attached to nothing, by name
    core::HashMap<flecs::entity_t, uint8_t> open_; // branches open (the value is unused)
    char filter_[96] = {};
    uint64_t next_read_ns_ = 0;
    bool rows_stale_ = true;
    char status_[64] = {};
    bool focused_ = false;
    flecs::entity_t revealed_ = 0;  // the selection last shown
    flecs::entity_t scroll_to_ = 0; // a row to bring into view
};

} // namespace de::editor
