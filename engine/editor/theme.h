#pragma once

#include <imgui.h>

#include <cstdint>

namespace de::platform {
class Vfs;
}

namespace de::editor {

// The editor's look (the user's own editor design, ported): colour roles, sizes at 100%, and the fonts. Panel
// code names a role, never a literal colour, and sizes in logical pixels through px().

// Colour roles.
enum class ThemeColor : uint8_t {
    Recessed,       // text, search and combo fields: darker than the panel around them
    Chrome,         // title and tab strips, gaps between panels, splitters
    Surface2,       // trees, navigation lists, alternate rows
    Surface,        // panel bodies, toolbars, the selected tab
    Header,         // category and column headers, panel footers, hovered rows
    Control,        // buttons and chips
    ControlHovered, //
    ControlActive,  //
    Border,         // popup and tooltip outlines
    Selection,      // selected rows in trees, lists and grids
    SelectionHovered,
    Accent, // primary or toggled-on state, focused panel tab, check marks
    AccentHovered,
    Text,
    TextEmphasis,
    TextSecondary,
    TextDisabled,
    AxisX,
    AxisY,
    AxisZ,
    Warning,
    Error,
    Success,
    Folder,
    TooltipBackground,
    TooltipText,
    TooltipBorder,
    Count,
};

// Sizes in logical pixels at 100% scale; the style scales them by the user's and the display's scale.
struct ThemeMetrics {
    float ui_scale = 1.0f; // the user's scale, on top of the display's
    float font_size = 13.0f;
    float row_height = 20.0f;
    float toolbar_height = 26.0f;
    float header_row_height = 30.0f; // the menu row
    float panel_gap = 2.0f;
    float control_rounding = 1.0f; // barely rounded: square corners read as a tool, round ones as a web page
    float popup_rounding = 1.0f;
};

struct Theme {
    uint32_t colors[static_cast<uint32_t>(ThemeColor::Count)] = {}; // 0xRRGGBB
    ThemeMetrics metrics;
};

// Neutral greys and one blue accent.
Theme default_theme();
const Theme& theme();

ImVec4 color(ThemeColor role, float alpha = 1.0f);
ImU32 color_u32(ThemeColor role, float alpha = 1.0f);

// The editor's 100% is ImGui's base size drawn 1.2 times larger.
inline constexpr float kBaseUiScale = 1.2f;

// Logical pixels at 100% to screen pixels at the user's and the display's scale.
float px(float logical);
// Screen pixels at 100% (authored at the base scale's size), at the user's and the display's scale.
float screen_px(float pixels);
float row_height();
float toolbar_height();
float header_row_height();
// Panels' and dialogs' content inset: docked panels stay edge to edge, their content doesn't.
float content_inset();
float content_vertical_inset();

struct Fonts {
    ImFont* regular = nullptr;
    ImFont* semibold = nullptr;
};
const Fonts& fonts();

// Loads Source Sans 3 (regular and semibold) from engine:/engine/editor/fonts and makes it ImGui's default. False
// (logged) if they're missing: ImGui's own font stays.
bool load_fonts(const platform::Vfs& vfs);

// Maps the theme onto Dear ImGui at the user's scale times the display's (content_scale: 1.5 at 150%). Always rebuilt
// from the 100% values, so changing scale never accumulates rounding.
void apply_style(float user_scale, float display_scale);

} // namespace de::editor
