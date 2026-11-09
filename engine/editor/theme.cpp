#include "editor/theme.h"

#include "core/allocator.h"
#include "core/log.h"
#include "platform/vfs.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace de::editor {

namespace {

struct Scale {
    float user = 1.0f;
    float display = 1.0f;
    float effective = kBaseUiScale;
};

Theme g_theme = default_theme();
Scale g_scale;
Fonts g_fonts;

float normalized_scale(float scale) {
    return scale > 0.0f && scale <= 16.0f ? scale : 1.0f;
}

// The font's own size; the style's FontSizeBase and scales draw it at the theme's.
constexpr float kFontPixels = 15.0f;

ImFont* load_font(const platform::Vfs& vfs, const char* path, const char* name) {
    auto bytes = vfs.read_whole_file(path, core::heap_allocator(core::MemoryTag::Editor));
    if (!bytes || bytes->empty()) {
        DE_LOG_WARN("editor", "no font %s: %s", path, bytes ? "empty" : bytes.error().message);
        return nullptr;
    }
    // The atlas keeps the data and frees it with ImGui's allocator.
    void* data = ImGui::MemAlloc(bytes->size());
    std::memcpy(data, bytes->data(), bytes->size());
    ImFontConfig config;
    config.SizePixels = kFontPixels;
    config.OversampleH = 2;
    config.OversampleV = 2;
    static_cast<void>(std::snprintf(config.Name, sizeof(config.Name), "%s", name));
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(bytes->size()), kFontPixels, &config);
}

} // namespace

Theme default_theme() {
    Theme theme;
    const auto set = [&theme](ThemeColor role, uint32_t rgb) { theme.colors[static_cast<uint32_t>(role)] = rgb; };
    set(ThemeColor::Recessed, 0x0f0f0fU);
    set(ThemeColor::Chrome, 0x151515U);
    set(ThemeColor::Surface2, 0x1a1a1aU);
    set(ThemeColor::Surface, 0x242424U);
    set(ThemeColor::Header, 0x2f2f2fU);
    set(ThemeColor::Control, 0x383838U);
    set(ThemeColor::ControlHovered, 0x454545U);
    set(ThemeColor::ControlActive, 0x575757U);
    set(ThemeColor::Border, 0x383838U);
    set(ThemeColor::Selection, 0x40576fU);
    set(ThemeColor::SelectionHovered, 0x4b6682U);
    set(ThemeColor::Accent, 0x0070e0U);
    set(ThemeColor::AccentHovered, 0x1a85f0U);
    set(ThemeColor::Text, 0xc0c0c0U);
    set(ThemeColor::TextEmphasis, 0xffffffU);
    set(ThemeColor::TextSecondary, 0x808080U);
    set(ThemeColor::TextDisabled, 0x575757U);
    set(ThemeColor::AxisX, 0xc83232U);
    set(ThemeColor::AxisY, 0x50b432U);
    set(ThemeColor::AxisZ, 0x3264dcU);
    set(ThemeColor::Warning, 0xe8a33cU);
    set(ThemeColor::Error, 0xe05252U);
    set(ThemeColor::Success, 0x5db85dU);
    set(ThemeColor::Folder, 0xb58a4aU);
    // Tooltips are light with a near-black outline: they never blend into a dark panel.
    set(ThemeColor::TooltipBackground, 0xf0f0f0U);
    set(ThemeColor::TooltipText, 0x1a1a1aU);
    set(ThemeColor::TooltipBorder, 0x151515U);
    return theme;
}

const Theme& theme() {
    return g_theme;
}

ImVec4 color(ThemeColor role, float alpha) {
    constexpr float kScale = 1.0f / 255.0f;
    const uint32_t rgb = g_theme.colors[static_cast<uint32_t>(role)];
    return {static_cast<float>((rgb >> 16U) & 0xffU) * kScale, static_cast<float>((rgb >> 8U) & 0xffU) * kScale,
            static_cast<float>(rgb & 0xffU) * kScale, alpha};
}

ImU32 color_u32(ThemeColor role, float alpha) {
    return ImGui::ColorConvertFloat4ToU32(color(role, alpha));
}

float px(float logical) {
    return logical * g_scale.effective;
}

float screen_px(float pixels) {
    return pixels * g_scale.user * g_scale.display;
}

float row_height() {
    return px(g_theme.metrics.row_height);
}

float toolbar_height() {
    return px(g_theme.metrics.toolbar_height);
}

float header_row_height() {
    return px(g_theme.metrics.header_row_height);
}

float content_inset() {
    return px(8.0f);
}

float content_vertical_inset() {
    return px(6.0f);
}

const Fonts& fonts() {
    return g_fonts;
}

bool load_fonts(const platform::Vfs& vfs) {
    ImFont* regular = load_font(vfs, "src:/engine/editor/fonts/source_sans_3_regular.ttf", "Source Sans 3 Regular");
    ImFont* semibold = load_font(vfs, "src:/engine/editor/fonts/source_sans_3_semibold.ttf", "Source Sans 3 Semibold");
    if (regular == nullptr || semibold == nullptr) {
        return false;
    }
    ImGuiIO& io = ImGui::GetIO();
    io.FontDefault = regular;
    // The style sets the display's scale on fonts with every other size; ImGui's own would overwrite it.
    io.ConfigDpiScaleFonts = false;
    g_fonts = {.regular = regular, .semibold = semibold};
    return true;
}

void apply_style(float user_scale, float display_scale) {
    const float user = normalized_scale(user_scale);
    const float display = normalized_scale(display_scale);
    const float effective = user * display * kBaseUiScale;
    const ThemeMetrics& metrics = g_theme.metrics;
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();

    const float row_padding = std::max(1.0f, (metrics.row_height - metrics.font_size) * 0.5f);
    style.FontSizeBase = metrics.font_size;
    style.DisabledAlpha = 0.6f;
    style.WindowPadding = {8.0f, 6.0f};
    style.WindowRounding = 0.0f;
    style.WindowBorderSize = 0.0f;
    style.WindowBorderHoverPadding = 6.0f;
    style.WindowTitleAlign = {0.0f, 0.5f};
    style.WindowMenuButtonPosition = ImGuiDir_None;
    style.ChildRounding = 0.0f;
    style.ChildBorderSize = 0.0f;
    style.PopupRounding = metrics.popup_rounding;
    style.PopupBorderSize = 1.0f;
    style.FramePadding = {6.0f, row_padding};
    style.FrameRounding = metrics.control_rounding;
    style.FrameBorderSize = 0.0f;
    style.ItemSpacing = {6.0f, 4.0f};
    style.ItemInnerSpacing = {4.0f, 4.0f};
    style.CellPadding = {6.0f, 3.0f};
    style.IndentSpacing = 16.0f;
    style.ColumnsMinSpacing = 4.0f;
    style.ScrollbarSize = 10.0f;
    style.ScrollbarRounding = metrics.control_rounding;
    style.ScrollbarPadding = 1.0f;
    style.GrabMinSize = 10.0f;
    style.GrabRounding = metrics.control_rounding;
    style.ImageRounding = 0.0f;
    style.TabRounding = 0.0f;
    style.TabBorderSize = 0.0f;
    style.TabMinWidthShrink = 48.0f;
    // The selected tab always shows its close button; other tabs show it on hover.
    style.TabCloseButtonMinWidthSelected = -1.0f;
    style.TabCloseButtonMinWidthUnselected = 0.0f;
    style.TabBarBorderSize = 0.0f;
    style.TabBarOverlineSize = 2.0f;
    style.TreeLinesFlags = ImGuiTreeNodeFlags_DrawLinesToNodes;
    style.TreeLinesSize = 1.0f;
    style.MenuItemRounding = 0.0f;
    style.SelectableRounding = 0.0f;
    style.DragDropTargetRounding = metrics.control_rounding;
    style.DragDropTargetBorderSize = 1.0f;
    style.ButtonTextAlign = {0.5f, 0.5f};
    style.SelectableTextAlign = {0.0f, 0.5f};
    style.SeparatorSize = 1.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.SeparatorTextPadding = {4.0f, 2.0f};
    // Docked panels are separated by a gap in the chrome colour, not by border lines.
    style.DockingSeparatorSize = metrics.panel_gap;

    using C = ThemeColor;
    ImVec4* const colors = style.Colors;
    colors[ImGuiCol_Text] = color(C::Text);
    colors[ImGuiCol_TextDisabled] = color(C::TextDisabled);
    colors[ImGuiCol_WindowBg] = color(C::Surface);
    colors[ImGuiCol_ChildBg] = color(C::Surface, 0.0f);
    colors[ImGuiCol_PopupBg] = color(C::Surface);
    colors[ImGuiCol_Border] = color(C::Border);
    colors[ImGuiCol_BorderShadow] = color(C::Recessed, 0.0f);
    colors[ImGuiCol_FrameBg] = color(C::Recessed);
    colors[ImGuiCol_FrameBgHovered] = color(C::Chrome);
    colors[ImGuiCol_FrameBgActive] = color(C::Recessed);
    colors[ImGuiCol_TitleBg] = color(C::Chrome);
    colors[ImGuiCol_TitleBgActive] = color(C::Chrome);
    colors[ImGuiCol_TitleBgCollapsed] = color(C::Chrome);
    colors[ImGuiCol_MenuBarBg] = color(C::Chrome);
    colors[ImGuiCol_ScrollbarBg] = color(C::Surface, 0.0f);
    // Grabs stand out from panel bodies: visible at rest, brighter on hover and drag.
    colors[ImGuiCol_ScrollbarGrab] = color(C::ControlActive);
    colors[ImGuiCol_ScrollbarGrabHovered] = color(C::TextSecondary);
    colors[ImGuiCol_ScrollbarGrabActive] = color(C::Text);
    colors[ImGuiCol_CheckMark] = color(C::Accent);
    colors[ImGuiCol_CheckboxSelectedBg] = color(C::Recessed);
    colors[ImGuiCol_SliderGrab] = color(C::ControlActive);
    colors[ImGuiCol_SliderGrabActive] = color(C::Accent);
    colors[ImGuiCol_Button] = color(C::Control);
    colors[ImGuiCol_ButtonHovered] = color(C::ControlHovered);
    colors[ImGuiCol_ButtonActive] = color(C::ControlActive);
    colors[ImGuiCol_Header] = color(C::Selection);
    colors[ImGuiCol_HeaderHovered] = color(C::Header);
    colors[ImGuiCol_HeaderActive] = color(C::SelectionHovered);
    colors[ImGuiCol_Separator] = color(C::Chrome);
    colors[ImGuiCol_SeparatorHovered] = color(C::ControlActive);
    colors[ImGuiCol_SeparatorActive] = color(C::Accent);
    colors[ImGuiCol_ResizeGrip] = color(C::Control, 0.0f);
    colors[ImGuiCol_ResizeGripHovered] = color(C::ControlActive);
    colors[ImGuiCol_ResizeGripActive] = color(C::Accent);
    colors[ImGuiCol_InputTextCursor] = color(C::TextEmphasis);
    colors[ImGuiCol_Tab] = color(C::Chrome);
    colors[ImGuiCol_TabHovered] = color(C::Header);
    colors[ImGuiCol_TabSelected] = color(C::Surface);
    colors[ImGuiCol_TabSelectedOverline] = color(C::Accent);
    colors[ImGuiCol_TabDimmed] = color(C::Chrome);
    colors[ImGuiCol_TabDimmedSelected] = color(C::Surface);
    colors[ImGuiCol_TabDimmedSelectedOverline] = color(C::Surface, 0.0f);
    colors[ImGuiCol_DockingPreview] = color(C::Accent, 0.35f);
    colors[ImGuiCol_DockingEmptyBg] = color(C::Chrome);
    colors[ImGuiCol_PlotLines] = color(C::Text);
    colors[ImGuiCol_PlotLinesHovered] = color(C::TextEmphasis);
    colors[ImGuiCol_PlotHistogram] = color(C::Accent);
    colors[ImGuiCol_PlotHistogramHovered] = color(C::AccentHovered);
    colors[ImGuiCol_TableHeaderBg] = color(C::Header);
    colors[ImGuiCol_TableBorderStrong] = color(C::Chrome);
    colors[ImGuiCol_TableBorderLight] = color(C::Chrome);
    colors[ImGuiCol_TableRowBg] = color(C::Surface, 0.0f);
    colors[ImGuiCol_TableRowBgAlt] = color(C::TextEmphasis, 0.025f);
    colors[ImGuiCol_TextLink] = color(C::AccentHovered);
    colors[ImGuiCol_TextSelectedBg] = color(C::Accent, 0.35f);
    colors[ImGuiCol_TreeLines] = color(C::Control);
    colors[ImGuiCol_DragDropTarget] = color(C::Accent);
    colors[ImGuiCol_DragDropTargetBg] = color(C::Accent, 0.15f);
    colors[ImGuiCol_UnsavedMarker] = color(C::TextEmphasis);
    colors[ImGuiCol_NavCursor] = color(C::Accent);
    colors[ImGuiCol_NavWindowingHighlight] = color(C::TextEmphasis, 0.7f);
    colors[ImGuiCol_NavWindowingDimBg] = color(C::Recessed, 0.3f);
    colors[ImGuiCol_ModalWindowDimBg] = color(C::Recessed, 0.55f);

    style.ScaleAllSizes(effective);
    style.FontScaleMain = user * kBaseUiScale;
    style.FontScaleDpi = display;
    g_scale = {.user = user, .display = display, .effective = effective};
}

} // namespace de::editor
