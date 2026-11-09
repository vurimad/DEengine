#pragma once

#include "editor/theme.h"

#include <imgui.h>

#include <cstddef>
#include <cstdint>

namespace de::editor {

// The editor's shared widgets (the user's own editor design, ported): every tooltip, arrow, menu, combo,
// category and property row in the editor goes through these, so they all look alike.

// A separator whose line reaches the edges of the region given in screen space, not just the content inset.
void structural_separator(float region_min_x, float region_max_x);

// The one tooltip look: light, outlined, wrapped. Never ImGui::SetTooltip.
bool begin_tooltip();
void end_tooltip();
template <typename... Args>
void tooltip(const char* format, Args... args) {
    if (format == nullptr || format[0] == '\0' || !begin_tooltip()) {
        return;
    }
    if constexpr (sizeof...(Args) == 0) {
        ImGui::TextWrapped("%s", format);
    } else {
        ImGui::TextWrapped(format, args...);
    }
    end_tooltip();
}

// A thin chevron (">" or "v") centred on center; extent is its half width.
void draw_chevron(ImDrawList* draw, ImVec2 center, float extent, ImGuiDir direction, ImU32 color);
// A circular arrow (refresh, reset): an open arc with an arrowhead on its end; a positive sweep turns clockwise.
void draw_circular_arrow(ImDrawList* draw, ImVec2 center, float radius, float start_angle, float sweep, ImU32 color,
                         float stroke);
// A small filled disclosure triangle, pointing right when closed and down when open.
void draw_disclosure(ImDrawList* draw, ImVec2 center, float extent, bool open, ImU32 color);
// Text in [position.x, max_x), ending in an ellipsis when it doesn't fit.
void draw_clipped_text(ImDrawList* draw, ImVec2 position, float max_x, const char* text, ImU32 color);
// A label inside [min, max), vertically centred, optionally horizontally; an ellipsis when it doesn't fit.
void draw_clipped_label(ImVec2 min, ImVec2 max, const char* label, ImU32 color, bool centered);

// A section heading inside a menu: dim capitals, then a rule to the menu's edge.
void menu_section(const char* caption);

// A recessed search field with a magnifier inside. True when the text changed.
bool search_field(const char* id, char* buffer, size_t capacity, const char* hint, float width);

// Menu bars, menus, context menus and combo lists share one look: a step above the panels, outlined, roomy rows.
void push_menu_style();
void pop_menu_style();
// Before BeginMenu or BeginCombo: menus get a comfortable minimum width.
void prepare_menu_popup();

// A combo whose arrow is a thin chevron inside the field. end_combo() only when it returned true.
bool begin_combo(const char* id, const char* preview, float width);
void end_combo();

// A collapsible category header: header colour, disclosure, semibold label. Its open state is kept per ID.
bool begin_category(const char* label, bool default_open = true);

// ---- Property rows: label | value | reset slot, thin dividers ----

struct PropertyRow {
    ImVec2 min;
    float width = 0.0f;
    float height = 0.0f;
    float value_x = 0.0f;   // the label/value divider
    float value_end = 0.0f; // where value widgets stop: the reset slot follows
};

float property_row_height();
// The space kept free at every row's right edge for the reset arrow.
float property_reset_slot();
// The width left for value widgets from the cursor to the reset slot.
float property_value_width(const PropertyRow& row);
// The label column's width for a row of width: the divider's fraction, clamped so neither column disappears.
float property_label_width(float width, float label_fraction);

// Lays out one row and leaves the cursor where the value goes. depth indents nested properties; with expanded, a
// disclosure triangle comes before the label and a click on the label side toggles it.
PropertyRow begin_property_row(const char* label, uint32_t depth = 0, bool* expanded = nullptr,
                               ThemeColor label_color = ThemeColor::Text, float label_fraction = 0.4f);
// Whether the mouse is over the row's label side (tooltips, context menus).
bool property_label_hovered(const PropertyRow& row);
// The thin dividers under the row and between its columns; the cursor moves to the next row.
void end_property_row(const PropertyRow& row);
// The reset arrow in the row's reset slot, shown only for a value that differs from its saved one. True when pressed.
bool property_reset_button(const PropertyRow& row, bool differs);

void section_header(const char* label);
void secondary_text(const char* text);

// ---- Resource glyphs ----

// A folder drawn in the folder colours, in [min, min + (width, height)).
void draw_folder_glyph(ImVec2 min, float width, float height);
// A page with a folded corner, extent square; with show_extension, the file's extension printed on it.
void draw_document_glyph(ImVec2 min, float extent, const char* name, bool show_extension);

} // namespace de::editor
