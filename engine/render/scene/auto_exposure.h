#pragma once

#include "core/result.h"
#include "render/graph/render_graph.h"
#include "render/rhi/rhi.h"

#include <cstdint>

namespace de::rhi {
class Device;
}
namespace de::shader {
class ShaderLibrary;
}

namespace de::render {

// Auto exposure (docs/design.md, Auto exposure): the picture's exposure follows how bright the scene is, as an eye
// adapts. A histogram of the frame's luminance on a log scale, centre-weighted
// (shaders/scene/exposure_histogram.slang); the mean of the pixels between two shares of them put at middle grey,
// within limits round the environment's own exposure, reached from last frame's at the eye's pace
// (exposure_adapt.slang). All on the GPU: the TAA and the tonemap read it the same frame, nothing comes back to the
// CPU.

struct AutoExposureSettings {
    float low_share = 0.3f;   // of the pixels, the darkest left out of the mean
    float high_share = 0.95f; // from where the brightest are left out
    float key = 0.18f;        // what the mean luminance becomes: middle grey
    float compensation_ev = 0.0f;
    // How far it goes from the environment's exposure toward what puts the scene at middle grey: an eye doesn't go
    // all the way, so a dark hall stays darker than a sunny yard. 1: all the way; 0: the environment's.
    float adaptation = 0.7f;
    // Round the environment's exposure (SceneLighting::exposure_ev): how much less and more it may give.
    float below_ev = 2.0f;
    float above_ev = 7.0f;
    float brighten_per_s = 1.2f; // toward more exposure (into the dark: the eye opens slowly)
    float darken_per_s = 3.0f;   // toward less (into the light: it closes fast)
};

// The histogram: kExposureBins bins over this range of log2 luminance, bin 0 for black.
inline constexpr uint32_t kExposureBins = 256;
inline constexpr float kExposureMinLog2 = -16.0f;
inline constexpr float kExposureMaxLog2 = 8.0f;

// What's kept on the GPU between frames (ExposureState in shaders/scene/exposure_adapt.slang).
struct GpuExposureState {
    float log2_exposure = 0.0f;
    uint32_t has_value = 0;
    float exposure = 1.0f; // what the TAA and the tonemap multiply by
    float target_log2 = 0.0f;
};
static_assert(sizeof(GpuExposureState) == 16);

struct AutoExposureInput {
    GraphTexture image; // the lit picture, before the TAA and the tonemap
    uint32_t width = 0;
    uint32_t height = 0;
    float base_ev = 0.0f; // the environment's exposure, which the limits are round
    // The environment's own limits and adaptation (AutoExposureSettings' below_ev, above_ev, adaptation), in place
    // of the settings'; negative: the settings'.
    float below_ev = -1.0f;
    float above_ev = -1.0f;
    float adaptation = -1.0f;
    float step_s = 0.0f; // since last frame: how far to adapt
};

struct AutoExposureDesc {
    rhi::Device* device = nullptr;
    shader::ShaderLibrary* shaders = nullptr;
};

struct AutoExposureState;

// On the thread that renders. The device and shader library must outlive it. Move-only.
class AutoExposure {
public:
    static core::Result<AutoExposure> create(const AutoExposureDesc& desc);

    ~AutoExposure();
    AutoExposure(AutoExposure&& other) noexcept;
    AutoExposure& operator=(AutoExposure&& other) noexcept;
    AutoExposure(const AutoExposure&) = delete;
    AutoExposure& operator=(const AutoExposure&) = delete;

    // Adds "exposure histogram" and "exposure adapt". Returns the GPU address of the exposure multiplier
    // (GpuExposureState::exposure) for the TAA and the tonemap, and the state's buffer they read it from; an
    // address of 0: none this frame (a shader missing), and the environment's exposure stands.
    struct Result {
        uint64_t exposure_address = 0;
        GraphBuffer state;
    };
    Result add_passes(RenderGraph& graph, const AutoExposureInput& input);

    // The next frame goes straight to its target, not at the eye's pace: after a cut, a new world.
    void snap();
    void set_settings(const AutoExposureSettings& settings);
    const AutoExposureSettings& settings() const;
    // The kept state, for tests to read back (CopySrc).
    rhi::BufferHandle state_buffer() const;

private:
    explicit AutoExposure(AutoExposureState* state) : state_(state) {}

    AutoExposureState* state_ = nullptr;
};

} // namespace de::render
