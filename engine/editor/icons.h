#pragma once

#include "core/allocator.h"
#include "core/array.h"
#include "core/math.h"

#include <imgui.h>

#include <cstdint>
#include <string_view>

namespace de::platform {
class Vfs;
}

namespace de::editor {

// The editor's icons: the user's own set, small SVGs in engine/editor/icons/ (a 24 x 24 view box, round strokes and
// flat fills), read once and drawn as vectors with ImGui's draw list, so they're sharp at any size and need no
// texture. What they use is what this reads: paths (M L H V C S A Z, absolute and relative), circles and rounded
// rectangles; a fill and a stroke from the root or the element; dashes.
enum class Icon : uint8_t {
    AlignToSurface,
    Camera,
    Center,
    Character,
    Eye,
    FastForward,
    FocusSelection,
    Group,
    Link,
    LocalCoords,
    Lock,
    Move,
    Pause,
    Pivot,
    Play,
    Redo,
    Rotate,
    Save,
    Scale,
    Select,
    SelectBox,
    SnapAngle,
    SnapGrid,
    SnapScale,
    SnapTerrain,
    StepForward,
    Stop,
    Terrain,
    Undo,
    Ungroup,
    Unlink,
    Unlock,
    WorldCoords,
    Zoom,
    Count,
};

class IconSet {
public:
    explicit IconSet(core::Allocator& allocator) : points_(allocator), shapes_(allocator) {}

    // Reads every icon from engine:/engine/editor/icons/ ("snap_grid.svg"). False (logged) if any is missing or doesn't
    // parse: those draw nothing.
    bool load(const platform::Vfs& vfs);
    // Reads one icon from an SVG's text. False if it doesn't parse.
    bool parse(Icon icon, std::string_view svg);
    // Draws an icon in the square at min, size wide, in color.
    void draw(ImDrawList* draw, Icon icon, ImVec2 min, float size, ImU32 color) const;
    // How many shapes an icon has (a path's every subpath, a circle, a rectangle): 0 if it didn't load.
    uint32_t shape_count(Icon icon) const { return icons_[static_cast<uint32_t>(icon)].shape_count; }

private:
    struct Shape {
        uint32_t first = 0; // into points_, in the view box's units
        uint32_t count = 0;
        float stroke_width = 0.0f; // 0: no stroke
        float dash = 0.0f;         // dashes: drawn, then a gap (0: a solid line)
        float gap = 0.0f;
        bool closed = false;
        bool fill = false;
    };
    struct Range {
        uint32_t first_shape = 0;
        uint32_t shape_count = 0;
        float view_size = 24.0f;
    };

    core::Array<Vec2> points_;
    core::Array<Shape> shapes_;
    Range icons_[static_cast<uint32_t>(Icon::Count)] = {};
};

} // namespace de::editor
