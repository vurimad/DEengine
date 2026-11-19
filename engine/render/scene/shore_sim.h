#pragma once

#include "core/math.h"
#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"
#include "render/scene/terrain.h"

#include <cstdint>

namespace de::rhi {
class Device;
}
namespace de::shader {
class ShaderLibrary;
}

namespace de::render {

// The surf, simulated (docs/design.md, Water; shaders/scene/shore_sim.slang): the shallow water equations on the GPU,
// in a window of kShoreSimSide cells kShoreSimCellM across around the camera, the seabed from the terrain, the sea's
// waves fed in at its edges. 8 MiB on the GPU.
inline constexpr uint32_t kShoreSimSide = 512;
inline constexpr float kShoreSimCellM = 0.5f;
inline constexpr double kShoreSimStepS = 1.0 / 240.0;
inline constexpr uint32_t kShoreSimMostSteps = 6; // a frame's; past it the surf runs slow rather than unstable

// Where the window is: its corner in cells of the world, and relative to a render origin.
struct ShoreSimWindow {
    int32_t corner_cell[2] = {};
    float corner[2] = {};
};

// The window around a camera (world space), the camera at its middle snapped to a cell.
ShoreSimWindow shore_sim_window(const DVec3& camera, const DVec3& origin);

struct ShoreSimInput {
    DVec3 camera; // world space
    DVec3 origin; // the frame's render origin
    double sea_level_m = 0.0;
    double time_s = 0.0; // the world's, at this frame: the water is stepped by how far it moved since the last
    uint64_t frame = 0;  // the frame's SceneFrame address
    uint32_t shore_material = 0;
    const TerrainHeights* terrain = nullptr; // none: the seabed from the shore map
    float wave_amplitude_m = 0.0f;
    float wave_period_s = 10.0f;
};

// This frame's surface, for the water to read once the pass has run: per cell (round a torus, a cell's texel its place
// in the world modulo kShoreSimSide), the surface's height about the sea's level, the depth, and the velocity x, z.
struct ShoreSimFrame {
    bool valid = false;
    GraphTexture surface;
    GraphTexture previous_surface; // last frame's, for motion; this frame's when there's none
    ShoreSimWindow window;
};

struct ShoreSimState;

class ShoreSim {
public:
    // Its textures and programs. Fails if a texture can't be made.
    static core::Result<ShoreSim> create(rhi::Device& device, shader::ShaderLibrary& shaders);

    ~ShoreSim();
    ShoreSim(ShoreSim&& other) noexcept;
    ShoreSim& operator=(ShoreSim&& other) noexcept;
    ShoreSim(const ShoreSim&) = delete;
    ShoreSim& operator=(const ShoreSim&) = delete;

    // Adds the passes that step the water to this frame: as many fixed steps as the time since the last frame holds.
    // Invalid, adding nothing, while its shaders are missing. A new sea (its level or shore map) starts it at rest.
    ShoreSimFrame add_pass(RenderGraph& graph, const ShoreSimInput& input);

    // The surfaces' textures: the one this frame's add_pass will write, and last frame's (that one again before there
    // was one): for the frame's constants, written before the passes are added.
    rhi::TextureHandle surface() const;
    rhi::TextureHandle previous_surface() const;
    // Whether its shaders are all there, so add_pass will run.
    bool running() const;

    // Forgets the water: the next frame starts it at rest.
    void reset();

private:
    explicit ShoreSim(ShoreSimState* state) : state_(state) {}

    ShoreSimState* state_ = nullptr;
};

} // namespace de::render
