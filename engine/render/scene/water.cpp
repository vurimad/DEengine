#include "render/scene/water.h"

#include "core/assert.h"

#include <cmath>

namespace de::render {

namespace {

constexpr uint32_t kSide = kWaterLevelQuads + 1;
constexpr uint32_t kHoleQuads = kWaterLevelQuads / 2;
constexpr uint32_t kFullIndices = kWaterLevelQuads * kWaterLevelQuads * 6;
constexpr uint32_t kRingIndices = ((kWaterLevelQuads * kWaterLevelQuads) - (kHoleQuads * kHoleQuads)) * 6;
static_assert(kSide * kSide <= 65536, "a level's vertices are indexed in 16 bits");
static_assert(kWaterLevelQuads % 4 == 0, "a hole is half a level, centered on a whole square");

// The hole's first square along an axis, off the middle by -1, 0 or 1.
constexpr uint32_t hole_start(int offset) {
    return static_cast<uint32_t>(static_cast<int>(kWaterLevelQuads / 4) + offset);
}

} // namespace

void water_levels(const DVec3& camera, const DVec3& origin, GpuWaterLevel (&out)[kWaterLevels]) {
    double middle_x[kWaterLevels];
    double middle_z[kWaterLevels];
    for (uint32_t level = 0; level < kWaterLevels; ++level) {
        const double spacing = static_cast<double>(kWaterNearSpacingM) * std::exp2(static_cast<double>(level));
        // On a grid of twice its spacing: its vertices then fall on every other one of the next level's.
        const double snap = 2.0 * spacing;
        middle_x[level] = std::round(camera.x / snap) * snap;
        middle_z[level] = std::round(camera.z / snap) * snap;
        GpuWaterLevel& gpu = out[level];
        gpu.center[0] = static_cast<float>(middle_x[level] - origin.x);
        gpu.center[1] = static_cast<float>(middle_z[level] - origin.z);
        gpu.spacing_m = static_cast<float>(spacing);
        gpu.hole = kWaterNoHole;
        if (level > 0) {
            // The level inside, in this level's squares: within one either way (water.h).
            const auto dx = static_cast<int>(std::lround((middle_x[level - 1] - middle_x[level]) / spacing));
            const auto dz = static_cast<int>(std::lround((middle_z[level - 1] - middle_z[level]) / spacing));
            DE_ASSERT(dx >= -1 && dx <= 1 && dz >= -1 && dz <= 1);
            gpu.hole = static_cast<uint32_t>((dx + 1) + (3 * (dz + 1)));
        }
    }
}

core::Array<uint16_t> water_mesh_indices(core::Allocator& allocator) {
    core::Array<uint16_t> indices(allocator);
    indices.reserve(kFullIndices + (kWaterHoles * kRingIndices));
    const auto vertex = [](uint32_t x, uint32_t z) { return static_cast<uint16_t>((z * kSide) + x); };
    const auto squares = [&](int hole_x, int hole_z, bool hole) {
        const uint32_t x0 = hole_start(hole_x);
        const uint32_t z0 = hole_start(hole_z);
        for (uint32_t z = 0; z < kWaterLevelQuads; ++z) {
            for (uint32_t x = 0; x < kWaterLevelQuads; ++x) {
                if (hole && x >= x0 && x < x0 + kHoleQuads && z >= z0 && z < z0 + kHoleQuads) {
                    continue;
                }
                // Counter-clockwise seen from above (+Y), with +Z toward the viewer: x right, z down the screen.
                for (const uint16_t index : {vertex(x, z), vertex(x, z + 1), vertex(x + 1, z + 1), vertex(x, z),
                                             vertex(x + 1, z + 1), vertex(x + 1, z)}) {
                    indices.push(index);
                }
            }
        }
    };
    squares(0, 0, false);
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dx = -1; dx <= 1; ++dx) {
            squares(dx, dz, true);
        }
    }
    return indices;
}

uint32_t water_first_index(uint32_t hole) {
    return hole >= kWaterHoles ? 0 : kFullIndices + (hole * kRingIndices);
}

uint32_t water_index_count(uint32_t hole) {
    return hole >= kWaterHoles ? kFullIndices : kRingIndices;
}

} // namespace de::render
