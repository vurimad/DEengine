#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/math.h"
#include "editor/selection.h"
#include "editor/widgets.h"

#include <flecs.h>
#include <imgui.h>

#include <cstddef>
#include <cstdint>

namespace de::app {
struct Engine;
}

namespace de::editor {

class CommandStack;

// The properties of what's selected, in the user's property grid look: a search field, then a category per
// component, a row per field (label | value | reset), a divider between the columns that drags. Fields come from
// the components' reflection (flecs meta): any registered member shows, nothing to write per component. Small
// vectors edit on one row with each axis' tint; a Transform's rotation shows as angles; enums are a list; nested
// structs open under their row. A reset arrow shows on fields edited this session, and puts back what they were.
// Every edit is a command (CommandStack): one drag is one undo. What the engine computes or holds for assets stays
// hidden until asked for (a link at the bottom), and then is read only. For a source file: what it is, and the asset it
// cooks to.
class PropertiesPanel {
public:
    static constexpr const char* kWindow = "Properties";

    explicit PropertiesPanel(core::Allocator& allocator) : scratch_(allocator) {}

    // True when the selected file was asked to be opened (a world's Open button).
    bool draw(const app::Engine& engine, Selection& selection, CommandStack& commands);

private:
    // Which widget changed the value this frame, and whether an edit ended.
    struct Edit {
        uint32_t item = 0;
        bool ended = false;
        bool reset = false; // a field went back to its saved value: its own command
    };
    // A component's value as it's drawn: its bytes (edited in place), what it was before this session's edits,
    // whether it may change.
    struct Value {
        std::byte* data = nullptr;
        const std::byte* saved = nullptr; // null: not edited, no reset arrows
        bool read_only = false;
    };

    void draw_entity(const app::Engine& engine, Selection& selection, CommandStack& commands);
    bool draw_file(const app::Engine& engine, const Selection& selection);
    // The rows of a struct's members; offset is where the struct starts in value's bytes.
    void draw_members(const flecs::world& ecs, flecs::entity_t type, const Value& value, size_t offset, uint32_t depth,
                      Edit& edit);
    // One field's row: a value of type (count of them) at offset in value's bytes.
    void draw_field(const flecs::world& ecs, const char* name, flecs::entity_t type, int32_t count, const Value& value,
                    size_t offset, uint32_t depth, Edit& edit);
    void draw_transform(const Value& value, flecs::entity_t entity, Edit& edit);
    // The label's tooltip after the hover delay, and the row's context menu.
    void row_extras(const PropertyRow& row, const char* label, const char* name, const char* type_name,
                    const char* value_text, bool can_reset, bool& reset);
    bool matches(const flecs::world& ecs, const char* name, flecs::entity_t type) const;

    core::Array<std::byte> scratch_; // the component's value while it's drawn
    char filter_[128] = {};
    float label_fraction_ = 0.4f;
    bool show_engine_ = false; // what the engine computes or owns shows too
    ImGuiID hovered_label_ = 0;
    double hover_start_s_ = 0.0;
    // The rotation's angles while they're dragged: read back from the rotation each frame, they could jump to another
    // set of angles for the same rotation.
    Vec3 rotation_deg_{0.0f};
    flecs::entity_t rotation_entity_ = 0;
    bool rotation_active_ = false;
};

} // namespace de::editor
