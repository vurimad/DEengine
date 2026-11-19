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

// ImGui::DockSpace, its splitters grabbed by the margin either side of the gap between panels as well as by the gap
// itself (ImGui lets the panels' own items take that margin from them).
void dock_space_with_splitters(ImGuiID id, ImVec2 size, ImGuiDockNodeFlags flags);

// A window over the docked panels (a drawer), at min and size: in front of them from the frame it appears on, as any
// window clicked is (the panels' host never comes in front), but not pushed in front every frame: that would put it
// over its own popups and take their clicks. End it with ImGui::End(), as Begin.
bool begin_over_panels(const char* name, ImVec2 min, ImVec2 size, ImGuiWindowFlags flags);

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
// How wide toggle_chip() draws a label.
float chip_width(const char* label);
// A toolbar that wraps: items a frame high laid left to right from a margin, onto another line when the next
// doesn't fit before limit_x (window coordinates, as ImGui::SetCursorPos takes). place() puts the cursor where the
// next item goes; bottom() is where what's under the toolbar starts.
class FlowRow {
public:
    FlowRow(float margin, float limit_x);
    // The next item, this wide, this far from the one before it on its line.
    void place(float item_width, float gap);
    float bottom() const;

private:
    float margin_;
    float limit_x_;
    float x_;
    float y_;
    bool first_ = true;
};
// A compact checkbox centred on a row's frame line (its ID is "##value": one to an ID scope). True when it changed.
bool compact_checkbox(bool& value);
// A word that's a switch or a button, a frame high: lit while on, a step lighter under the mouse, greyed while
// disabled (ImGui::BeginDisabled). Its tip shows either way. True when pressed.
bool toggle_chip(const char* label, bool on, const char* tip);

// ---- Resource glyphs ----

// A folder drawn in the folder colours, in [min, min + (width, height)).
void draw_folder_glyph(ImVec2 min, float width, float height);
// A page with a folded corner, extent square; with show_extension, the file's extension printed on it.
void draw_document_glyph(ImVec2 min, float extent, const char* name, bool show_extension);

} // namespace de::editor
