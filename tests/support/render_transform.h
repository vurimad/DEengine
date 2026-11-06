#pragma once

#include "world/render_extract.h"

namespace de::test {

// From a 3x4 matrix (rows x, y, z, the world position in the last column). A float position is coarser than the
// engine's doubles, which is fine for what tests place.
inline de::world::RenderTransform render_transform(const float (&matrix)[12]) {
    de::world::RenderTransform out;
    for (int row = 0; row < 3; ++row) {
        out.rows[row].linear[0] = matrix[row * 4 + 0];
        out.rows[row].linear[1] = matrix[row * 4 + 1];
        out.rows[row].linear[2] = matrix[row * 4 + 2];
        out.set_position_m(row, static_cast<double>(matrix[row * 4 + 3]));
    }
    return out;
}

} // namespace de::test
