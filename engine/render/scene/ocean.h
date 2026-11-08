#pragma once

#include "core/math.h"
#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/scene/water.h"

#include <cstdint>

namespace de::rhi {
class Device;
}
namespace de::shader {
class ShaderLibrary;
}

namespace de::render {

// The FFT ocean (docs/design.md, Water; shaders/scene/ocean.slang): the sea's waves, brought to each frame's time on
// the GPU. Three square tiles of kOceanSize texels, kOceanTileM across, from the swell's down to ripples, each holding
// the waves the others don't; the water's mesh sums them where it lies. 18 MiB on the GPU.
inline constexpr uint32_t kOceanSize = 256;
inline constexpr uint32_t kOceanCascades = 3;
inline constexpr uint32_t kOceanMips = 9; // down to a texel
inline constexpr float kOceanTileM[kOceanCascades] = {640.0f, 101.0f, 17.3f};

// JONSWAP's numbers for a sea state: what the spectrum is made from.
struct OceanSpectrum {
    float wind_alpha = 0.0f;
    float wind_peak = 0.0f; // rad/s
    float wind_direction[2] = {1.0f, 0.0f};
    float swell_alpha = 0.0f;
    float swell_peak = 0.0f;
    float swell_direction[2] = {0.0f, -1.0f};
    float wind_spread = 0.0f; // normalizations of the spreading
    float swell_spread = 0.0f;
    float choppiness = 0.0f;

    bool operator==(const OceanSpectrum&) const = default;
};

// The wind's waves over their fetch (fully grown past a dimensionless fetch of about 22,000), the swell's scaled to
// its significant height, both spread about their ways.
OceanSpectrum ocean_spectrum(const SceneSea& sea, float wind_speed_mps, const float (&wind_direction)[2]);

// This frame's waves, for the water to read, once the pass has run.
struct OceanFrame {
    bool valid = false;
    GraphTexture waves;          // per cascade: displacement x, y, z and the Jacobian, with mips
    GraphTexture previous_waves; // last frame's, for motion (this frame's, at first: has_previous false)
    bool has_previous = false;
    GraphTexture slopes;         // per cascade: slope x, z, their square's mean and foam, with mips
    float tile_offset[kOceanCascades][2] = {}; // where the render origin is in each tile, as a share of it (x, z)
};

struct OceanState;

class Ocean {
public:
    // Its textures and programs. Fails if a texture can't be made.
    static core::Result<Ocean> create(rhi::Device& device, shader::ShaderLibrary& shaders);

    ~Ocean();
    Ocean(Ocean&& other) noexcept;
    Ocean& operator=(Ocean&& other) noexcept;
    Ocean(const Ocean&) = delete;
    Ocean& operator=(const Ocean&) = delete;

    // Adds the pass that brings the waves to time_s (the world's), made again from the spectrum if the sea's state or
    // the wind changed. Invalid, adding nothing, while its shaders are missing.
    OceanFrame add_pass(RenderGraph& graph, const SceneSea& sea, float wind_speed_mps, const float (&wind_direction)[2],
                        double time_s, double frame_s, const DVec3& origin);

private:
    explicit Ocean(OceanState* state) : state_(state) {}

    OceanState* state_ = nullptr;
};

} // namespace de::render
