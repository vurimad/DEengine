#include "editor/widgets.h"

#include <imgui_internal.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>

namespace de::editor {

namespace {

// The folder and document glyphs' colours.
constexpr ImU32 kFolderBody = IM_COL32(165, 127, 72, 255);
constexpr ImU32 kFolderTab = IM_COL32(143, 105, 57, 255);
constexpr ImU32 kFolderHighlight = IM_COL32(188, 149, 88, 180);
constexpr ImU32 kFolderMicroBody = IM_COL32(188, 149, 88, 255);
constexpr ImU32 kFolderBorder = IM_COL32(82, 61, 37, 255);
constexpr ImU32 kFolderShadow = IM_COL32(0, 0, 0, 72);
constexpr ImU32 kDocumentBody = IM_COL32(104, 104, 104, 255);
constexpr ImU32 kDocumentFold = IM_COL32(154, 154, 154, 255);
constexpr ImU32 kDocumentBorder = IM_COL32(184, 184, 184, 255);
constexpr ImU32 kDocumentText = IM_COL32(242, 242, 242, 255);

float stroke(float logical) {
    return std::max(1.0f, px(logical));
}

} // namespace

void structural_separator(float region_min_x, float region_max_x) {
    ImGui::Separator();
    if (region_max_x <= region_min_x) {
        return;
    }
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    if (max.y <= min.y) {
        return;
    }
    ImGui::PushClipRect(ImVec2(region_min_x, min.y), ImVec2(region_max_x, max.y), false);
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(region_min_x, min.y), ImVec2(region_max_x, max.y),
                                              ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::PopClipRect();
}

bool begin_tooltip() {
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8.0f), px(5.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(6.0f), px(4.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, stroke(1.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(theme().metrics.popup_rounding));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f); // a tooltip never scrolls: wrapped text sizes it
    ImGui::PushStyleColor(ImGuiCol_PopupBg, color(ThemeColor::TooltipBackground));
    ImGui::PushStyleColor(ImGuiCol_Border, color(ThemeColor::TooltipBorder));
    ImGui::PushStyleColor(ImGuiCol_Text, color(ThemeColor::TooltipText));
    ImGui::PushStyleColor(ImGuiCol_Separator, color(ThemeColor::TooltipBorder));
    ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f), ImVec2(px(380.0f), FLT_MAX));
    if (!ImGui::BeginTooltip()) {
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(5);
        return false;
    }
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + px(360.0f));
    return true;
}

void end_tooltip() {
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(5);
}

void draw_chevron(ImDrawList* draw, ImVec2 center, float extent, ImGuiDir direction, ImU32 color) {
    const float width = stroke(1.4f);
    const float half = extent * 0.5f;
    if (direction == ImGuiDir_Down) {
        draw->AddLine(ImVec2(center.x - extent, center.y - half), ImVec2(center.x, center.y + half), color, width);
        draw->AddLine(ImVec2(center.x, center.y + half), ImVec2(center.x + extent, center.y - half), color, width);
    } else {
        draw->AddLine(ImVec2(center.x - half, center.y - extent), ImVec2(center.x + half, center.y), color, width);
        draw->AddLine(ImVec2(center.x + half, center.y), ImVec2(center.x - half, center.y + extent), color, width);
    }
}

void draw_circular_arrow(ImDrawList* draw, ImVec2 center, float radius, float start_angle, float sweep, ImU32 color,
                         float stroke_width) {
    const float end_angle = start_angle + sweep;
    draw->PathArcTo(center, radius, start_angle, end_angle, 24);
    draw->PathStroke(color, ImDrawFlags_None, stroke_width);
    const ImVec2 normal(std::cos(end_angle), std::sin(end_angle));
    const float direction = sweep >= 0.0f ? 1.0f : -1.0f;
    const ImVec2 tangent(-normal.y * direction, normal.x * direction);
    const ImVec2 end(center.x + (normal.x * radius), center.y + (normal.y * radius));
    const float arrow = radius * 0.62f;
    draw->AddTriangleFilled(ImVec2(end.x + (tangent.x * arrow), end.y + (tangent.y * arrow)),
                            ImVec2(end.x + (normal.x * arrow * 0.6f) - (tangent.x * arrow * 0.2f),
                                   end.y + (normal.y * arrow * 0.6f) - (tangent.y * arrow * 0.2f)),
                            ImVec2(end.x - (normal.x * arrow * 0.6f) - (tangent.x * arrow * 0.2f),
                                   end.y - (normal.y * arrow * 0.6f) - (tangent.y * arrow * 0.2f)),
                            color);
}

void draw_disclosure(ImDrawList* draw, ImVec2 center, float extent, bool open, ImU32 color) {
    if (open) {
        draw->AddTriangleFilled(ImVec2(center.x - extent, center.y - (extent * 0.55f)),
                                ImVec2(center.x + extent, center.y - (extent * 0.55f)),
                                ImVec2(center.x, center.y + (extent * 0.6f)), color);
    } else {
        draw->AddTriangleFilled(ImVec2(center.x - (extent * 0.55f), center.y - extent),
                                ImVec2(center.x + (extent * 0.6f), center.y),
                                ImVec2(center.x - (extent * 0.55f), center.y + extent), color);
    }
}

void draw_clipped_text(ImDrawList* draw, ImVec2 position, float max_x, const char* text, ImU32 color) {
    if (max_x <= position.x || text == nullptr) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::RenderTextEllipsis(draw, position, ImVec2(max_x, position.y + ImGui::GetFontSize()), max_x, text, nullptr,
                              nullptr);
    ImGui::PopStyleColor();
}

void draw_clipped_label(ImVec2 min, ImVec2 max, const char* label, ImU32 color, bool centered) {
    if (label == nullptr || max.x <= min.x || max.y <= min.y) {
        return;
    }
    const ImVec2 size = ImGui::CalcTextSize(label);
    const float available = max.x - min.x;
    const float x = centered && size.x <= available ? min.x + ((available - size.x) * 0.5f) : min.x;
    const float y = min.y + std::max(0.0f, (max.y - min.y - size.y) * 0.5f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::RenderTextEllipsis(ImGui::GetWindowDrawList(), ImVec2(x, y), max, max.x, label, nullptr, &size);
    ImGui::PopStyleColor();
}

void menu_section(const char* caption) {
    char upper[96];
    size_t length = 0;
    for (const char* at = caption; *at != '\0' && length + 1 < sizeof(upper); ++at) {
        upper[length++] = *at >= 'a' && *at <= 'z' ? static_cast<char>(*at - 32) : *at;
    }
    upper[length] = '\0';
    const ImVec2 position = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFontSize() + px(6.0f);
    ImGui::Dummy(ImVec2(ImGui::CalcTextSize(upper).x, height));
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float text_y = std::floor(position.y + ((height - ImGui::GetFontSize()) * 0.5f));
    constexpr float kScale = 0.85f;
    const float size = ImGui::GetFontSize() * kScale;
    draw->AddText(ImGui::GetFont(), size, ImVec2(position.x, text_y + (ImGui::GetFontSize() * (1.0f - kScale) * 0.5f)),
                  color_u32(ThemeColor::TextSecondary), upper);
    const float text_width = ImGui::GetFont()->CalcTextSizeA(size, FLT_MAX, 0.0f, upper).x;
    const float line_y = std::floor(position.y + (height * 0.5f)) + 0.5f;
    const float right = ImGui::GetWindowPos().x + ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x;
    if (right > position.x + text_width + px(8.0f)) {
        draw->AddLine(ImVec2(position.x + text_width + px(8.0f), line_y), ImVec2(right, line_y),
                      color_u32(ThemeColor::Control), stroke(1.0f));
    }
}

bool search_field(const char* id, char* buffer, size_t capacity, const char* hint, float width) {
    const float icon_space = ImGui::GetFrameHeight();
    ImGui::SetNextItemWidth(width > 0.0f ? width : ImGui::GetContentRegionAvail().x);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(icon_space, ImGui::GetStyle().FramePadding.y));
    const bool changed = ImGui::InputTextWithHint(id, hint, buffer, capacity);
    ImGui::PopStyleVar();
    const ImVec2 field = ImGui::GetItemRectMin();
    const float extent = ImGui::GetItemRectSize().y;
    const ImVec2 center(field.x + (icon_space * 0.5f) - px(1.0f), field.y + (extent * 0.5f) - px(1.0f));
    const float radius = extent * 0.2f;
    const float width_px = stroke(1.4f);
    const ImU32 ink = color_u32(ImGui::IsItemActive() ? ThemeColor::Text : ThemeColor::TextSecondary);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddCircle(center, radius, ink, 16, width_px);
    draw->AddLine(ImVec2(center.x + (radius * 0.72f), center.y + (radius * 0.72f)),
                  ImVec2(center.x + (radius * 1.6f), center.y + (radius * 1.6f)), ink, width_px);
    return changed;
}

void push_menu_style() {
    // A menu row is a little taller than a list row: its pitch is the font plus the item spacing.
    const float pitch = row_height() + px(4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(px(8.0f), px(6.0f)));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(px(12.0f), std::max(0.0f, pitch - ImGui::GetFontSize())));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupRounding, px(theme().metrics.popup_rounding));
    ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, stroke(1.0f));
    // Selectable rows reach half the item spacing out; without this an auto-fitted menu shows a scrollbar.
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_PopupBg, color(ThemeColor::Header));
    ImGui::PushStyleColor(ImGuiCol_Border, color(ThemeColor::Control));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, color(ThemeColor::ControlHovered));
    ImGui::PushStyleColor(ImGuiCol_Separator, color(ThemeColor::Control));
}

void pop_menu_style() {
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(5);
}

void prepare_menu_popup() {
    ImGui::SetNextWindowSizeConstraints(ImVec2(px(220.0f), 0.0f), ImVec2(FLT_MAX, FLT_MAX));
}

bool begin_combo(const char* id, const char* preview, float width) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const float height = ImGui::GetFrameHeight();
    const float field_width = width > 0.0f ? width : ImGui::GetContentRegionAvail().x;
    ImGui::SetNextItemWidth(field_width);
    push_menu_style();
    ImGui::SetNextWindowSizeConstraints(ImVec2(field_width, 0.0f), ImVec2(FLT_MAX, FLT_MAX));
    const bool open = ImGui::BeginCombo(id, "", ImGuiComboFlags_NoArrowButton);
    // The preview stops before the chevron, in an ellipsis when the field is narrow.
    const ImVec2 text(min.x + ImGui::GetStyle().FramePadding.x,
                      std::floor(min.y + ((height - ImGui::GetFontSize()) * 0.5f)));
    const float text_end = min.x + field_width - (height * 0.9f);
    if (text_end > text.x) {
        draw_clipped_text(draw, text, text_end, preview, color_u32(ThemeColor::Text));
    }
    draw_chevron(draw, ImVec2(min.x + field_width - (height * 0.5f), min.y + (height * 0.5f)), height * 0.16f,
                 ImGuiDir_Down, color_u32(ThemeColor::TextSecondary));
    if (!open) {
        pop_menu_style();
    }
    return open;
}

void end_combo() {
    ImGui::EndCombo();
    pop_menu_style();
}

bool begin_category(const char* label, bool default_open) {
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID id = ImGui::GetID(label);
    bool open = storage->GetBool(id, default_open);
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 size(std::max(1.0f, ImGui::GetContentRegionAvail().x), row_height() + px(2.0f));
    ImGui::PushID(label);
    if (ImGui::InvisibleButton("##category", size)) {
        open = !open;
        storage->SetBool(id, open);
    }
    const bool hovered = ImGui::IsItemHovered();
    ImGui::PopID();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, ImVec2(min.x + size.x, min.y + size.y),
                        color_u32(hovered ? ThemeColor::ControlHovered : ThemeColor::Header));
    draw_disclosure(draw, ImVec2(min.x + px(12.0f), min.y + (size.y * 0.5f)), px(3.5f), open,
                    color_u32(ThemeColor::TextSecondary));
    if (fonts().semibold != nullptr) {
        ImGui::PushFont(fonts().semibold, 0.0f);
    }
    draw->AddText(ImVec2(min.x + px(24.0f), std::floor(min.y + ((size.y - ImGui::GetFontSize()) * 0.5f))),
                  color_u32(ThemeColor::TextEmphasis), label);
    if (fonts().semibold != nullptr) {
        ImGui::PopFont();
    }
    return open;
}

float property_row_height() {
    return row_height() + px(2.0f);
}

float property_reset_slot() {
    return ImGui::GetFrameHeight() + px(4.0f);
}

float property_value_width(const PropertyRow& row) {
    return std::max(1.0f, row.value_end - ImGui::GetCursorScreenPos().x);
}

float property_label_width(float width, float label_fraction) {
    // In narrow panels both columns shrink and their contents clip in place.
    const float min_label = std::min(px(48.0f), width * 0.5f);
    const float max_label = std::max(min_label, width - property_reset_slot() - px(32.0f));
    return std::floor(std::clamp(width * label_fraction, min_label, max_label));
}

PropertyRow begin_property_row(const char* label, uint32_t depth, bool* expanded, ThemeColor label_color,
                               float label_fraction) {
    PropertyRow row;
    row.min = ImGui::GetCursorScreenPos();
    row.width = ImGui::GetContentRegionAvail().x;
    row.height = property_row_height();
    row.value_x = row.min.x + property_label_width(row.width, label_fraction);
    row.value_end = std::max(row.value_x + px(8.0f), row.min.x + row.width - property_reset_slot());
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float label_x = row.min.x + px(20.0f + (12.0f * static_cast<float>(depth)));
    const float label_y = std::floor(row.min.y + ((row.height - ImGui::GetFontSize()) * 0.5f));
    if (expanded != nullptr) {
        ImGui::PushID(label);
        ImGui::SetCursorScreenPos(row.min);
        if (ImGui::InvisibleButton("##expand", ImVec2(std::max(1.0f, row.value_x - row.min.x), row.height))) {
            *expanded = !*expanded;
        }
        ImGui::PopID();
        draw_disclosure(draw, ImVec2(label_x - px(9.0f), row.min.y + (row.height * 0.5f)), px(3.0f), *expanded,
                        color_u32(ThemeColor::TextSecondary));
    }
    draw_clipped_text(draw, ImVec2(label_x, label_y), row.value_x - px(6.0f), label, color_u32(label_color));
    ImGui::SetCursorScreenPos(
        ImVec2(row.value_x + px(6.0f), row.min.y + std::floor((row.height - ImGui::GetFrameHeight()) * 0.5f)));
    return row;
}

bool property_label_hovered(const PropertyRow& row) {
    return ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
           ImGui::IsMouseHoveringRect(row.min, ImVec2(row.value_x, row.min.y + row.height));
}

void end_property_row(const PropertyRow& row) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 line = color_u32(ThemeColor::Surface2);
    const float thickness = stroke(1.0f);
    const float bottom = row.min.y + row.height;
    draw->AddRectFilled(ImVec2(row.min.x, bottom - thickness), ImVec2(row.min.x + row.width, bottom), line);
    draw->AddRectFilled(ImVec2(row.value_x, row.min.y), ImVec2(row.value_x + thickness, bottom), line);
    ImGui::SetCursorScreenPos(row.min);
    ImGui::Dummy(ImVec2(row.width, row.height));
}

bool property_reset_button(const PropertyRow& row, bool differs) {
    if (!differs) {
        return false;
    }
    const float extent = ImGui::GetFrameHeight();
    const ImVec2 min(std::floor(row.value_end + ((property_reset_slot() - extent) * 0.5f)),
                     row.min.y + std::floor((row.height - extent) * 0.5f));
    ImGui::SetCursorScreenPos(min);
    const bool pressed = ImGui::InvisibleButton("##reset", ImVec2(extent, extent));
    const bool hovered = ImGui::IsItemHovered();
    const ImU32 ink = color_u32(hovered ? ThemeColor::TextEmphasis : ThemeColor::TextSecondary);
    draw_circular_arrow(ImGui::GetWindowDrawList(), ImVec2(min.x + (extent * 0.5f), min.y + (extent * 0.5f)),
                        extent * 0.24f, 2.25f, -4.75f, ink, stroke(1.3f));
    if (hovered) {
        tooltip("Revert to saved: as it was before your edits");
    }
    return pressed;
}

void section_header(const char* label) {
    if (fonts().semibold != nullptr) {
        ImGui::PushFont(fonts().semibold, 0.0f);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, color(ThemeColor::TextEmphasis));
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
    if (fonts().semibold != nullptr) {
        ImGui::PopFont();
    }
}

void secondary_text(const char* text) {
    if (text != nullptr && text[0] != '\0') {
        ImGui::TextColored(color(ThemeColor::TextSecondary), "%s", text);
    }
}

void draw_folder_glyph(ImVec2 min, float width, float height) {
    if (width <= 0.0f || height <= 0.0f) {
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const bool micro = height < 18.0f;
    const float inset = width * 0.025f;
    const float body_top = min.y + (height * 0.26f);
    const ImVec2 body_min(min.x + inset, body_top);
    const ImVec2 body_max(min.x + width - inset, min.y + height);
    const ImVec2 tab_min(body_min.x + (width * 0.035f), min.y + (height * 0.08f));
    const ImVec2 tab_max(min.x + (width * 0.43f), body_top + (height * 0.06f));
    const float line = std::clamp(height * 0.035f, 0.65f, 1.5f);
    const float body_rounding = micro ? 0.0f : std::min(1.5f, height * 0.06f);
    const float tab_rounding = micro ? 0.0f : std::min(1.0f, height * 0.05f);
    if (!micro) {
        const float shadow = std::clamp(height * 0.025f, 0.75f, 2.0f);
        draw->AddRectFilled(ImVec2(body_min.x + shadow, body_min.y + shadow),
                            ImVec2(body_max.x + shadow, body_max.y + shadow), kFolderShadow, body_rounding);
    }
    draw->AddRectFilled(tab_min, tab_max, micro ? kFolderBody : kFolderTab, tab_rounding);
    draw->AddRectFilled(body_min, body_max, micro ? kFolderMicroBody : kFolderBody, body_rounding);
    if (!micro) {
        const float line_inset = std::max(line, width * 0.035f);
        draw->AddLine(ImVec2(body_min.x + line_inset, body_min.y + line),
                      ImVec2(body_max.x - line_inset, body_min.y + line), kFolderHighlight, line);
        draw->AddRect(body_min, body_max, kFolderBorder, body_rounding, 0, line);
    }
}

void draw_document_glyph(ImVec2 min, float extent, const char* name, bool show_extension) {
    if (extent <= 0.0f) {
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float page_height = extent * 0.88f;
    const float page_width = extent * 0.70f;
    const float fold = page_width * 0.24f;
    const ImVec2 page_min(min.x + ((extent - page_width) * 0.5f), min.y + ((extent - page_height) * 0.5f));
    const ImVec2 page_max(page_min.x + page_width, page_min.y + page_height);
    const ImVec2 points[] = {page_min, ImVec2(page_max.x - fold, page_min.y), ImVec2(page_max.x, page_min.y + fold),
                             page_max, ImVec2(page_min.x, page_max.y)};
    draw->AddConvexPolyFilled(points, 5, kDocumentBody);
    const ImVec2 fold_points[] = {ImVec2(page_max.x - fold, page_min.y), ImVec2(page_max.x - fold, page_min.y + fold),
                                  ImVec2(page_max.x, page_min.y + fold)};
    draw->AddConvexPolyFilled(fold_points, 3, kDocumentFold);
    draw->AddPolyline(points, 5, kDocumentBorder, ImDrawFlags_Closed, std::clamp(extent * 0.018f, 0.75f, 1.5f));
    if (!show_extension || name == nullptr) {
        return;
    }
    const char* dot = std::strrchr(name, '.');
    if (dot == nullptr || dot[1] == '\0') {
        return;
    }
    char label[6] = {};
    const size_t length = std::min(std::strlen(dot + 1), sizeof(label) - 1);
    std::memcpy(label, dot + 1, length);
    const float size = ImGui::GetFontSize() * 0.72f;
    const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(size, page_width - 4.0f, 0.0f, label);
    draw->AddText(
        ImGui::GetFont(), size,
        ImVec2(page_min.x + ((page_width - text.x) * 0.5f), page_min.y + (page_height * 0.58f) - (text.y * 0.5f)),
        kDocumentText, label);
}

bool begin_over_panels(const char* name, ImVec2 min, ImVec2 size, ImGuiWindowFlags flags) {
    ImGui::SetNextWindowPos(min);
    ImGui::SetNextWindowSize(size);
    const bool shown = ImGui::Begin(name, nullptr, flags);
    if (ImGui::IsWindowAppearing()) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    }
    return shown;
}

void dock_space_with_splitters(ImGuiID id, ImVec2 size, ImGuiDockNodeFlags flags) {
    // The splitters are made here, before the panels' contents. ImGui marks them overlappable, so a later item under
    // the mouse would take the hover: what a splitter takes here, it keeps.
    ImGuiContext& context = *ImGui::GetCurrentContext();
    const ImGuiID hovered_before = context.HoveredId;
    ImGui::DockSpace(id, size, flags);
    if (context.HoveredId != 0 && context.HoveredId != hovered_before) {
        context.HoveredIdAllowOverlap = false;
    }
}

} // namespace de::editor
