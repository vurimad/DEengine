#pragma once

#include "core/math.h"
#include "editor/selection.h"

#include <cstdint>

namespace de::app {
struct Engine;
}

namespace de::editor {

// The cell map: the world from above, as it's stored and streamed. The terrain's extent, the grid of 128 m cells,
// the cells that are loaded (filled), the camera (a dot and where it looks) and the selection. The wheel zooms about
// the mouse, the right or middle button drags it about; it follows the camera until it's dragged (Center brings it
// back). A click on a cell selects it; a double-click sends the camera there, at its height, looking as it was.
class CellMapPanel {
public:
    static constexpr const char* kWindow = "Cell Map";

    // True when a place was double-clicked: the camera should go there (world X and Z in to).
    bool draw(const app::Engine& engine, Selection& selection, DVec3& to);
    // What the status bar shows while the panel has focus: "1 cell loaded  camera in cell [0, 0]".
    const char* status() const { return status_; }
    bool focused() const { return focused_; }

private:
    double center_x_m_ = 0.0;
    double center_z_m_ = 0.0;
    float pixels_per_m_ = 0.5f;
    bool follow_ = true; // the view stays on the camera
    char status_[96] = {};
    bool focused_ = false;
};

} // namespace de::editor
